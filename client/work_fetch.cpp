// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2008 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

#include "cpp.h"

#ifdef _WIN32
#include "boinc_win.h"
#else
#include "config.h"
#endif

#include "util.h"

#include "client_msgs.h"
#include "client_state.h"
#include "project.h"
#include "result.h"
#include "scheduler_op.h"

#include "work_fetch.h"

#if 0
#define WF_DEBUG(x) x
#else
#define WF_DEBUG(X)
#endif

using std::vector;

RSC_WORK_FETCH rsc_work_fetch[MAX_RSC];
WORK_FETCH work_fetch;

static inline int dont_fetch(PROJECT* p, int rsc_type) {
    if (p->no_rsc_pref[rsc_type]) return DONT_FETCH_PREFS;
    if (p->no_rsc_config[rsc_type]) return DONT_FETCH_CONFIG;
    if (p->no_rsc_apps[rsc_type]) return DONT_FETCH_NO_APPS;
    if (p->no_rsc_ams[rsc_type]) return DONT_FETCH_AMS;
    return 0;
}

// if the configuration file disallows the use of a GPU type
// for a project, set a flag to that effect
//
void set_no_rsc_config() {
    for (unsigned int i=0; i<gstate.projects.size(); i++) {
        PROJECT& p = *gstate.projects[i];
        for (int j=1; j<coprocs.n_rsc; j++) {
            bool allowed[MAX_COPROC_INSTANCES];
            memset(allowed, 0, sizeof(allowed));
            COPROC& c = coprocs.coprocs[j];
            for (int k=0; k<c.count; k++) {
                allowed[c.device_nums[k]] = true;
            }
            for (unsigned int k=0; k<cc_config.exclude_gpus.size(); k++) {
                EXCLUDE_GPU& e = cc_config.exclude_gpus[k];
                if (strcmp(e.url.c_str(), p.master_url)) continue;
                if (!e.type.empty() && strcmp(e.type.c_str(), c.type)) continue;
                if (!e.appname.empty()) continue;
                if (e.device_num < 0) {
                    memset(allowed, 0, sizeof(allowed));
                    break;
                }
                allowed[e.device_num] = false;
            }
            p.no_rsc_config[j] = true;
            for (int k=0; k<c.count; k++) {
                if (allowed[c.device_nums[k]]) {
                    p.no_rsc_config[j] = false;
                    break;
                }
            }
        }
    }
}

// does the (NCI) project have a job that's running or uploading?
// (don't request another job from NCI project if so)
//
static bool has_a_job_in_progress(PROJECT* p) {
    for (unsigned int j=0; j<gstate.results.size(); j++) {
        RESULT* rp = gstate.results[j];
        if (rp->project != p) continue;
        if (rp->state() < RESULT_FILES_UPLOADED) {
            return true;
        }
    }
    return false;
}

inline bool has_coproc_app(PROJECT* p, int rsc_type) {
    unsigned int i;
    for (i=0; i<gstate.app_versions.size(); i++) {
        APP_VERSION* avp = gstate.app_versions[i];
        if (avp->project != p) continue;
        if (avp->gpu_usage.rsc_type == rsc_type) return true;
    }
    return false;
}

///////////////  RSC_PROJECT_WORK_FETCH  ///////////////

bool RSC_PROJECT_WORK_FETCH::compute_may_have_work(PROJECT* p, int rsc_type) {
    if (dont_fetch(p, rsc_type)) return false;
    if (p->rsc_defer_sched[rsc_type]) return false;
    return (backoff_time < gstate.now);
}

void RSC_PROJECT_WORK_FETCH::rr_init(PROJECT* p, int rsc_type) {
    may_have_work = compute_may_have_work(p, rsc_type);
    fetchable_share = 0;
    n_runnable_jobs = 0;
    sim_nused = 0;
    nused_total = 0;
    deadlines_missed = 0;
}

void RSC_PROJECT_WORK_FETCH::resource_backoff(PROJECT* p, const char* name) {
    if (backoff_interval) {
        backoff_interval *= 2;
        if (backoff_interval > WF_MAX_BACKOFF_INTERVAL) backoff_interval = WF_MAX_BACKOFF_INTERVAL;
    } else {
        backoff_interval = WF_MIN_BACKOFF_INTERVAL;
    }
    double x = (.5 + drand())*backoff_interval;
    backoff_time = gstate.now + x;
    if (log_flags.work_fetch_debug) {
        msg_printf(p, MSG_INFO,
            "[work_fetch] backing off %s %.0f sec", name, x
        );
    }
}

///////////////  RSC_WORK_FETCH  ///////////////

RSC_PROJECT_WORK_FETCH& RSC_WORK_FETCH::project_state(PROJECT* p) {
    return p->rsc_pwf[rsc_type];
}

void RSC_WORK_FETCH::rr_init() {
    shortfall = 0;
    nidle_now = 0;
    sim_nused = 0;
    total_fetchable_share = 0;
    deadline_missed_instances = 0;
    saturated_time = 0;
    busy_time_estimator.reset();
    sim_used_instances = 0;
}

void RSC_WORK_FETCH::update_stats(double sim_now, double dt, double buf_end) {
    double idle = ninstances - sim_nused;
    if (idle > 1e-6 && sim_now < buf_end) {
        double dt2;
        if (sim_now + dt > buf_end) {
            dt2 = buf_end - sim_now;
        } else {
            dt2 = dt;
        }
        shortfall += idle*dt2;
    }
    if (idle < 1e-6) {
        saturated_time = sim_now + dt - gstate.now;
    }
}

void RSC_WORK_FETCH::update_busy_time(double dur, double nused) {
    busy_time_estimator.update(dur, nused);
}

static bool wacky_dcf(PROJECT* p) {
    if (p->dont_use_dcf) return false;
    double dcf = p->duration_correction_factor;
    return (dcf < 0.02 || dcf > 80.0);
}

// request this project's share of shortfall and instances.
// don't request anything if project is backed off.
//
void RSC_WORK_FETCH::set_request(PROJECT* p) {

    // if backup project, fetch 1 job per idle instance
    //
    if (p->resource_share == 0) {
        req_instances = nidle_now;
        req_secs = 1;
        return;
    }
    if (cc_config.fetch_minimal_work) {
        req_instances = ninstances;
        req_secs = 1;
        return;
    }
    RSC_PROJECT_WORK_FETCH& w = project_state(p);
    double non_excl_inst = ninstances - w.ncoprocs_excluded;
    if (shortfall) {
        if (wacky_dcf(p)) {
            // if project's DCF is too big or small,
            // its completion time estimates are useless; just ask for 1 second
            //
            req_secs = 1;
        } else {
            req_secs = shortfall;
            if (w.ncoprocs_excluded) {
                req_secs *= non_excl_inst/ninstances;
            }
        }
    }

    double instance_share = ninstances*w.fetchable_share;
    if (instance_share > non_excl_inst) {
        instance_share = non_excl_inst;
    }
    instance_share -= w.nused_total;
    req_instances = std::max(nidle_now, instance_share);

    if (log_flags.work_fetch_debug) {
        msg_printf(p, MSG_INFO,
            "[work_fetch] set_request() for %s: ninst %d nused_total %.2f nidle_now %.2f fetch share %.2f req_inst %.2f req_secs %.2f",
            rsc_name_long(rsc_type), ninstances, w.nused_total, nidle_now,
            w.fetchable_share, req_instances, req_secs
        );
    }
    if (req_instances && !req_secs) {
        req_secs = 1;
    }
}

// We're fetching work because some instances are starved because
// of exclusions.
// See how many N of these instances are not excluded for this project.
// Ask for N instances and for N*work_buf_min seconds.
//
void RSC_WORK_FETCH::set_request_excluded(PROJECT* p) {
    RSC_PROJECT_WORK_FETCH& pwf = project_state(p);

    int inst_mask = sim_excluded_instances & pwf.non_excluded_instances;
    int n = 0;
    for (int i=0; i<ninstances; i++) {
        if ((1<<i) & inst_mask) {
            n++;
        }
    }
    WF_DEBUG(msg_printf(p, MSG_INFO, "set_request_excluded() %d %d %d", sim_excluded_instances, pwf.non_excluded_instances, n));
    req_instances = n;
    if (p->resource_share == 0 || cc_config.fetch_minimal_work) {
        req_secs = 1;
    } else {
        req_secs = n*gstate.work_buf_total();
    }
}

void RSC_WORK_FETCH::print_state(const char* name) {
    msg_printf(0, MSG_INFO, "[work_fetch] --- state for %s ---", name);
    msg_printf(0, MSG_INFO,
        "[work_fetch] shortfall %.2f nidle %.2f saturated %.2f busy %.2f",
        shortfall, nidle_now, saturated_time,
        busy_time_estimator.get_busy_time()
    );
    msg_printf(0, MSG_INFO, "[work_fetch] sim used inst %d sim excl inst %d",
        sim_used_instances, sim_excluded_instances
    );
    for (unsigned int i=0; i<gstate.projects.size(); i++) {
        char buf[256];
        PROJECT* p = gstate.projects[i];
        if (p->non_cpu_intensive) continue;
        RSC_PROJECT_WORK_FETCH& pwf = project_state(p);
        bool no_rsc_pref = p->no_rsc_pref[rsc_type];
        bool no_rsc_config = p->no_rsc_config[rsc_type];
        bool no_rsc_apps = p->no_rsc_apps[rsc_type];
        bool no_rsc_ams = p->no_rsc_ams[rsc_type];
        double bt = pwf.backoff_time>gstate.now?pwf.backoff_time-gstate.now:0;
        if (bt) {
            sprintf(buf, " (resource backoff: %.2f, inc %.2f)",
                bt, pwf.backoff_interval
            );
        } else {
            strcpy(buf, "");
        }
        msg_printf(p, MSG_INFO,
            "[work_fetch] fetch share %.3f%s%s%s%s%s",
            pwf.fetchable_share,
            buf,
            no_rsc_pref?" (blocked by prefs)":"",
            no_rsc_apps?" (no apps)":"",
            no_rsc_ams?" (blocked by account manager)":"",
            no_rsc_config?" (blocked by configuration file)":""
        );
    }
}

void RSC_WORK_FETCH::clear_request() {
    req_secs = 0;
    req_instances = 0;
}

///////////////  PROJECT_WORK_FETCH  ///////////////

int PROJECT_WORK_FETCH::compute_cant_fetch_work_reason(PROJECT* p) {
    if (p->non_cpu_intensive) return CANT_FETCH_WORK_NON_CPU_INTENSIVE;
    if (p->suspended_via_gui) return CANT_FETCH_WORK_SUSPENDED_VIA_GUI;
    if (p->master_url_fetch_pending) return CANT_FETCH_WORK_MASTER_URL_FETCH_PENDING;
    if (p->dont_request_more_work) return CANT_FETCH_WORK_DONT_REQUEST_MORE_WORK;
    if (p->some_download_stalled()) return CANT_FETCH_WORK_DOWNLOAD_STALLED;
    if (p->some_result_suspended()) return CANT_FETCH_WORK_RESULT_SUSPENDED;
    if (p->too_many_uploading_results) return CANT_FETCH_WORK_TOO_MANY_UPLOADS;

    // this goes last
    //
    if (p->min_rpc_time > gstate.now) return CANT_FETCH_WORK_MIN_RPC_TIME;
    return 0;
}

void PROJECT_WORK_FETCH::reset(PROJECT* p) {
    for (int i=0; i<coprocs.n_rsc; i++) {
        p->rsc_pwf[i].reset();
    }
}

///////////////  WORK_FETCH  ///////////////

// mark the projects from which we can fetch work
//
void WORK_FETCH::compute_cant_fetch_work_reason() {
    for (unsigned int i=0; i<gstate.projects.size(); i++) {
        PROJECT* p = gstate.projects[i];
        p->pwf.cant_fetch_work_reason = p->pwf.compute_cant_fetch_work_reason(p);
    }
}

void WORK_FETCH::rr_init() {
    for (int i=0; i<coprocs.n_rsc; i++) {
        rsc_work_fetch[i].rr_init();
    }
    compute_cant_fetch_work_reason();
    for (unsigned int i=0; i<gstate.projects.size(); i++) {
        PROJECT* p = gstate.projects[i];
        p->pwf.n_runnable_jobs = 0;
        for (int j=0; j<coprocs.n_rsc; j++) {
            p->rsc_pwf[j].rr_init(p, j);
        }
    }
}

#if 0
// if the given project is highest-priority among the projects
// eligible for the resource, set request fields
//
void RSC_WORK_FETCH::supplement(PROJECT* pp) {
    double x = pp->sched_priority;
    for (unsigned i=0; i<gstate.projects.size(); i++) {
        PROJECT* p = gstate.projects[i];
        if (p == pp) continue;
        if (p->pwf.cant_fetch_work_reason) continue;
        if (!project_state(p).may_have_work) continue;
        RSC_PROJECT_WORK_FETCH& rpwf = project_state(p);
        if (rpwf.anon_skip) continue;
        if (p->sched_priority > x) {
            if (log_flags.work_fetch_debug) {
                msg_printf(pp, MSG_INFO,
                    "[work_fetch]: not requesting work for %s: %s has higher priority",
                    rsc_name_long(rsc_type), p->get_project_name()
                );
            }
            return;
        }
    }
    // didn't find a better project; ask for work
    //
    set_request(pp);
}

// we're going to ask the given project for work of the given type.
// (or -1 if none)
// Set requests for this type and perhaps other types
//
void WORK_FETCH::set_all_requests_hyst(PROJECT* p, int rsc_type) {
    for (int i=0; i<coprocs.n_rsc; i++) {
        if (i == rsc_type) {
            rsc_work_fetch[i].set_request(p);
        } else {
            // don't fetch work for a resource if the buffer is above max
            //
            if (rsc_work_fetch[i].saturated_time > gstate.work_buf_total()) {
                continue;
            }
            
            // don't fetch work if backup project and no idle instances
            //
            if (p->resource_share==0 && rsc_work_fetch[i].nidle_now==0) {
                continue;
            }

            if (i>0 && !gpus_usable) {
                continue;
            }
            rsc_work_fetch[i].supplement(p);
        }
    }
}

void WORK_FETCH::set_all_requests(PROJECT* p) {
    for (int i=0; i<coprocs.n_rsc; i++) {
        if (i==0 || gpus_usable) {
            rsc_work_fetch[i].set_request(p);
        }
    }
}
#endif

void WORK_FETCH::print_state() {
    msg_printf(0, MSG_INFO, "[work_fetch] ------- start work fetch state -------");
    msg_printf(0, MSG_INFO, "[work_fetch] target work buffer: %.2f + %.2f sec",
        gstate.work_buf_min(), gstate.work_buf_additional()
    );
    msg_printf(0, MSG_INFO, "[work_fetch] --- project states ---");
    for (unsigned int i=0; i<gstate.projects.size(); i++) {
        PROJECT* p = gstate.projects[i];
        char buf[1024], buf2[1024];
        if (p->pwf.cant_fetch_work_reason) {
            sprintf(buf, "can't req work: %s", cant_fetch_work_string(p, buf2));
        } else {
            strcpy(buf, "can req work");
        }
        if (p->min_rpc_time > gstate.now) {
            sprintf(buf2, " (backoff: %.2f sec)", p->min_rpc_time - gstate.now);
            strcat(buf, buf2);
        }
        msg_printf(p, MSG_INFO, "[work_fetch] REC %.3f prio %.6f %s",
            p->pwf.rec,
            p->sched_priority,
            buf
        );
    }
    for (int i=0; i<coprocs.n_rsc; i++) {
        rsc_work_fetch[i].print_state(rsc_name_long(i));
    }
    msg_printf(0, MSG_INFO, "[work_fetch] ------- end work fetch state -------");
}

void WORK_FETCH::clear_request() {
    for (int i=0; i<coprocs.n_rsc; i++) {
        rsc_work_fetch[i].clear_request();
    }
}

bool WORK_FETCH::requested_work() {
    for (int i=0; i<coprocs.n_rsc; i++) {
        if (rsc_work_fetch[i].req_secs) return true;
    }
    return false;
}

// We're going to contact this project for reasons other than work fetch
// (e.g., to report completed results, or at user request).
// Decide if we should "piggyback" a work fetch request.
//
void WORK_FETCH::piggyback_work_request(PROJECT* p) {
    WF_DEBUG(msg_printf(p, MSG_INFO, "piggyback_work_request()");)
    clear_request();
    if (cc_config.fetch_minimal_work && gstate.had_or_requested_work) return;
    if (p->non_cpu_intensive) {
        if (!has_a_job_in_progress(p) && !p->dont_request_more_work) {
            rsc_work_fetch[0].req_secs = 1;
        }
        return;
    }

    setup();

    switch (p->pwf.cant_fetch_work_reason) {
    case 0:
    case CANT_FETCH_WORK_MIN_RPC_TIME:
        break;
    default:
        return;
    }

    // if project was updated from manager and config says so,
    // fetch work for a resource even if there are higher-prio projects
    // able to fetch it
    //
    bool check_higher_priority_projects = true;
    if (p->sched_rpc_pending && cc_config.fetch_on_update) {
        check_higher_priority_projects = false;
    }

    // For each resource, scan projects in decreasing priority,
    // seeing if there's one that's higher-priority than this
    // able to fetch work for the resource.
    // If not, and the resource needs topping off, do so
    //
    for (int i=0; i<coprocs.n_rsc; i++) {
        WF_DEBUG(msg_printf(p, MSG_INFO, "piggyback: resource %s", rsc_name_long(i));)
        RSC_WORK_FETCH& rwf = rsc_work_fetch[i];
        if (i && !gpus_usable) {
            rwf.dont_fetch_reason = DONT_FETCH_GPUS_NOT_USABLE;
            continue;
        }
        rwf.dont_fetch_reason = rwf.cant_fetch(p);
        if (rwf.dont_fetch_reason) {
            WF_DEBUG(msg_printf(p, MSG_INFO, "piggyback: can't fetch %s", rsc_name_long(i));)
            continue;
        }
        bool buffer_low = (rwf.saturated_time < gstate.work_buf_total());
        bool need_work = buffer_low;
        if (rwf.has_exclusions && rwf.uses_starved_excluded_instances(p)) {
            need_work = true;
        }
        if (!need_work) {
            WF_DEBUG(msg_printf(p, MSG_INFO, "piggyback: don't need %s", rsc_name_long(i));)
            rwf.dont_fetch_reason = DONT_FETCH_BUFFER_FULL;
            continue;
        }
        if (check_higher_priority_projects) {
            PROJECT* p2 = NULL;
            for (unsigned int j=0; j<gstate.projects.size(); j++) {
                p2 = gstate.projects[j];
                if (p2 == p) break;
				if (p2->sched_priority == p->sched_priority) continue;
                if (p2->pwf.cant_fetch_work_reason) {
                    WF_DEBUG(msg_printf(p, MSG_INFO, "piggyback: %s can't fetch work", p2->project_name);)
                    continue;
                }
                if (!rwf.cant_fetch(p2) && !rwf.backed_off(p2)) {
                    WF_DEBUG(msg_printf(p, MSG_INFO, "piggyback: better proj %s", p2->project_name);)
                    break;
                }
            }
            if (p != p2) {
                rwf.dont_fetch_reason = DONT_FETCH_NOT_HIGHEST_PRIO;
                continue;
            }
        }
        WF_DEBUG(msg_printf(p, MSG_INFO, "piggyback: requesting %s", rsc_name_long(i));)
        if (buffer_low) {
            rwf.set_request(p);
        } else {
            rwf.set_request_excluded(p);
        }
    }
    if (!requested_work()) {
        p->pwf.cant_fetch_work_reason = CANT_FETCH_WORK_DONT_NEED;
    }
}

// see if there's a fetchable non-CPU-intensive project without work
//
PROJECT* WORK_FETCH::non_cpu_intensive_project_needing_work() {
    for (unsigned int i=0; i<gstate.projects.size(); i++) {
        PROJECT* p = gstate.projects[i];
        if (!p->non_cpu_intensive) continue;
        if (!p->can_request_work()) continue;
        if (p->rsc_pwf[0].backoff_time > gstate.now) continue;
        if (has_a_job_in_progress(p)) continue;
        clear_request();
        rsc_work_fetch[0].req_secs = 1;
        return p;
    }
    return 0;
}

static bool higher_priority(PROJECT *p1, PROJECT *p2) {
    return (p1->sched_priority > p2->sched_priority);
}

// check resource-level backoff
//
bool RSC_WORK_FETCH::backed_off(PROJECT* p) {
    if (project_state(p).backoff_time > gstate.now) {
        WF_DEBUG(msg_printf(p, MSG_INFO, "skip: backoff");)
        return true;
    }
    return false;
}

// a variety of checks for whether we should ask this project
// for work of this type
//
int RSC_WORK_FETCH::cant_fetch(PROJECT *p) {
    // see whether work fetch for this resource is banned
    // by prefs, config, project, or acct mgr
    //
    int reason = dont_fetch(p, rsc_type);
    if (reason) {
        WF_DEBUG(msg_printf(p, MSG_INFO, "skip: dont_fetch");)
        return reason;
    }

    RSC_PROJECT_WORK_FETCH& rpwf = project_state(p);

    // if project has zero resource share,
    // only fetch work if a device is idle
    //
    if (p->resource_share == 0 && nidle_now == 0 && sim_excluded_instances==0) {
        WF_DEBUG(msg_printf(p, MSG_INFO, "skip: zero share");)
        return DONT_FETCH_ZERO_SHARE;
    }

    // if project has excluded GPUs of this type,
    // we need to avoid fetching work just because there's an idle instance
    // or a shortfall;
    // fetching work might not alleviate either of these,
    // and we'd end up fetching unbounded work.
    // At the same time, we want to respect work buf params if possible.
    //
    // Current policy:
    // don't fetch work if remaining time of this project's jobs
    // exceeds work_buf_min * (#usable instances / #instances)
    //
    // TODO: THIS IS FAIRLY CRUDE. Making it smarter would require
    // computing shortfall etc. on a per-project basis
    //
    int nexcl = rpwf.ncoprocs_excluded;
    if (rsc_type && nexcl) {
        int n_not_excluded = ninstances - nexcl;
        if (rpwf.n_runnable_jobs >= n_not_excluded
            && rpwf.queue_est > (gstate.work_buf_min() * n_not_excluded)/ninstances
        ) {
            WF_DEBUG(msg_printf(p, MSG_INFO, "skip: too much work");)
            return DONT_FETCH_BUFFER_FULL;
        }
    }

    if (rpwf.anon_skip) {
        WF_DEBUG(msg_printf(p, MSG_INFO, "skip: anon");)
        return DONT_FETCH_NO_APPS;
    }
    return 0;
}

// return true if there is exclusion starvation
// and this project can use the starved instances
//
bool RSC_WORK_FETCH::uses_starved_excluded_instances(PROJECT* p) {
    RSC_PROJECT_WORK_FETCH& rpwf = project_state(p);
    if (!sim_excluded_instances) return false;
    if ((sim_excluded_instances & rpwf.non_excluded_instances) == 0) {
        WF_DEBUG(msg_printf(p, MSG_INFO, "skip: excl");)
        return false;
    }
    return true;
}

// setup for choose_project() and piggyback()
//
void WORK_FETCH::setup() {
    gstate.compute_nuploading_results();

    rr_simulation();
    compute_shares();
    project_priority_init(true);
    clear_request();

    // Decrement the priority of projects that have work queued.
    // Specifically, subtract
    // (FLOPs queued for P)/(FLOPs of max queue)
    // which will generally be between 0 and 1.
    // This is a little arbitrary but I can't think of anything better.
    //
    double max_queued_flops = gstate.work_buf_total()*total_peak_flops();
    for (unsigned int i=0; i<gstate.results.size(); i++) {
        RESULT* rp = gstate.results[i];
        PROJECT* p = rp->project;
        p->sched_priority -= rp->estimated_flops_remaining()/max_queued_flops;
    }

    // don't request work from projects w/ > 1000 runnable jobs
    //
    int job_limit = 1000;
    for (unsigned int i=0; i<gstate.projects.size(); i++) {
        PROJECT* p = gstate.projects[i];
        if (p->pwf.n_runnable_jobs > job_limit && !p->pwf.cant_fetch_work_reason) {
            p->pwf.cant_fetch_work_reason = CANT_FETCH_WORK_TOO_MANY_RUNNABLE;
        }
    }

    std::sort(
        gstate.projects.begin(),
        gstate.projects.end(),
        higher_priority
    );
    if (log_flags.work_fetch_debug) {
        print_state();
    }
}

// Choose a project to fetch work from,
// and set the request fields of resource objects.
// Set p->sched_rpc_pending; if you decide not to request work
// from the project, you must clear this.
//
PROJECT* WORK_FETCH::choose_project() {
    PROJECT* p;

    if (log_flags.work_fetch_debug) {
        msg_printf(0, MSG_INFO, "[work_fetch] entering choose_project()");
    }

    p = non_cpu_intensive_project_needing_work();
    if (p) return p;

    setup();

    for (int i=0; i<coprocs.n_rsc; i++) {
        rsc_work_fetch[i].found_project = NULL;
    }

    // scan projects in order of decreasing priority
    //
    bool found = false;
    for (unsigned int j=0; j<gstate.projects.size(); j++) {
        p = gstate.projects[j];
        WF_DEBUG(msg_printf(p, MSG_INFO, "scanning");)
        if (p->pwf.cant_fetch_work_reason) {
            WF_DEBUG(msg_printf(p, MSG_INFO, "skip: cfwr %d", p->pwf.cant_fetch_work_reason);)
            continue;
        }

        // For each resource type:
        // - See if we can ask this project for work of that type;
        //   if so set a flag so that lower-priority projects
        //   won't request it
        // - If so, see if work is needed for this type;
        //   if so, set "found_project" flag
        //
        int rsc_index = -1;
        for (int i=0; i<coprocs.n_rsc; i++) {
            if (i && !gpus_usable) continue;
            RSC_WORK_FETCH& rwf = rsc_work_fetch[i];
            if (!rwf.cant_fetch(p) && !rwf.backed_off(p)) {
                if (!rwf.found_project) {
                    rwf.found_project = p;
                }
                WF_DEBUG(msg_printf(p, MSG_INFO, "can fetch %s", rsc_name_long(i));)
            } else {
                WF_DEBUG(msg_printf(p, MSG_INFO, "can't fetch %s", rsc_name_long(i));)
                continue;
            }
            if (rwf.saturated_time < gstate.work_buf_min()) {
                WF_DEBUG(msg_printf(p, MSG_INFO, "%s needs work - buffer low", rsc_name_long(i));)
                rsc_index = i;
                break;
            }
            if (rwf.has_exclusions && rwf.uses_starved_excluded_instances(p)) {
                WF_DEBUG(msg_printf(p, MSG_INFO, "%s needs work - excluded instance starved", rsc_name_long(i));)
                rsc_index = i;
                break;
            }
        }

        // If rsc_index is nonzero, it's a resource that this project
        // can ask for work, and which needs work.
        // And this is the highest-priority project having this property.
        // Request work from this resource,
        // and any others for which this is the highest-priority project
        // able to request work
        //
        if (rsc_index >= 0) {
            bool any_request = false;
            for (int i=0; i<coprocs.n_rsc; i++) {
                if (i && !gpus_usable) continue;
                RSC_WORK_FETCH& rwf = rsc_work_fetch[i];
                bool buffer_low;
                WF_DEBUG(msg_printf(p, MSG_INFO, "checking %s", rsc_name_long(i));)
                if (i == rsc_index) {
                    buffer_low = (rwf.saturated_time < gstate.work_buf_min());
                } else {
                    if (rwf.found_project && rwf.found_project != p) {
                        WF_DEBUG(msg_printf(p, MSG_INFO, "%s not high prio proj", rsc_name_long(i));)
                        continue;
                    }
                    buffer_low = (rwf.saturated_time < gstate.work_buf_total());
                    bool need_work = buffer_low;
                    if (rwf.has_exclusions && rwf.uses_starved_excluded_instances(p)) {
                        need_work = true;
                    }
                    if (!need_work) {
                        WF_DEBUG(msg_printf(p, MSG_INFO, "%s don't need", rsc_name_long(i));)
                        continue;
                    }
                    int reason = rwf.cant_fetch(p);
                    if (reason) {
                        WF_DEBUG(msg_printf(p, MSG_INFO, "%s can't fetch", rsc_name_long(i));)
                        continue;
                    }
                }
                if (buffer_low) {
                    rwf.set_request(p);
                    WF_DEBUG(msg_printf(p, MSG_INFO, "%s set_request: %f", rsc_name_long(i), rwf.req_secs);)
                } else {
                    rwf.set_request_excluded(p);
                    WF_DEBUG(msg_printf(p, MSG_INFO, "%s set_request_excluded: %f", rsc_name_long(i), rwf.req_secs);)
                }
                if (rwf.req_secs > 0) {
                    any_request = true;
                }
            }
            if (any_request) {
                found = true;
                break;
            }
        }
    }

    if (found) {
        p->sched_rpc_pending = RPC_REASON_NEED_WORK;
    } else {
        if (log_flags.work_fetch_debug) {
            msg_printf(0, MSG_INFO, "[work_fetch] No project chosen for work fetch");
        }
        p = NULL;
    }

    return p;
}

void WORK_FETCH::accumulate_inst_sec(ACTIVE_TASK* atp, double dt) {
    APP_VERSION* avp = atp->result->avp;
    PROJECT* p = atp->result->project;
    double x = dt*avp->avg_ncpus;
    p->rsc_pwf[0].secs_this_rec_interval += x;
    rsc_work_fetch[0].secs_this_rec_interval += x;
    int rt = avp->gpu_usage.rsc_type;
    if (rt) {
        x = dt*avp->gpu_usage.usage;
        p->rsc_pwf[rt].secs_this_rec_interval += x;
        rsc_work_fetch[rt].secs_this_rec_interval += x;
    }
}

// find total and per-project resource shares for each resource
//
void WORK_FETCH::compute_shares() {
    unsigned int i;
    PROJECT* p;
    for (i=0; i<gstate.projects.size(); i++) {
        p = gstate.projects[i];
        if (p->non_cpu_intensive) continue;
        if (p->pwf.cant_fetch_work_reason) continue;
        for (int j=0; j<coprocs.n_rsc; j++) {
            if (p->rsc_pwf[j].may_have_work) {
                rsc_work_fetch[j].total_fetchable_share += p->resource_share;
            }
        }
    }
    for (i=0; i<gstate.projects.size(); i++) {
        p = gstate.projects[i];
        if (p->non_cpu_intensive) continue;
        if (p->pwf.cant_fetch_work_reason) continue;
        for (int j=0; j<coprocs.n_rsc; j++) {
            if (p->rsc_pwf[j].may_have_work) {
                p->rsc_pwf[j].fetchable_share = rsc_work_fetch[j].total_fetchable_share?p->resource_share/rsc_work_fetch[j].total_fetchable_share:1;
            }
        }
    }
}

void WORK_FETCH::request_string(char* buf) {
    char buf2[256];
    sprintf(buf,
        "[work_fetch] request: CPU (%.2f sec, %.2f inst)",
        rsc_work_fetch[0].req_secs, rsc_work_fetch[0].req_instances
    );
    for (int i=1; i<coprocs.n_rsc; i++) {
        sprintf(buf2, " %s (%.2f sec, %.2f inst)",
            rsc_name_long(i), rsc_work_fetch[i].req_secs, rsc_work_fetch[i].req_instances
        );
        strcat(buf, buf2);
    }
}

void WORK_FETCH::write_request(FILE* f, PROJECT* p) {
    double work_req = rsc_work_fetch[0].req_secs;

    // if project is anonymous platform, set the overall work req
    // to the max of the requests of resource types for which we have versions.
    // Otherwise projects with old schedulers won't send us work.
    // THIS CAN BE REMOVED AT SOME POINT
    //
    if (p->anonymous_platform) {
        for (int i=1; i<coprocs.n_rsc; i++) {
            if (has_coproc_app(p, i)) {
                if (rsc_work_fetch[i].req_secs > work_req) {
                    work_req = rsc_work_fetch[i].req_secs;
                }
            }
        }
    }
    fprintf(f,
        "    <work_req_seconds>%f</work_req_seconds>\n"
        "    <cpu_req_secs>%f</cpu_req_secs>\n"
        "    <cpu_req_instances>%f</cpu_req_instances>\n"
        "    <estimated_delay>%f</estimated_delay>\n",
        work_req,
        rsc_work_fetch[0].req_secs,
        rsc_work_fetch[0].req_instances,
        rsc_work_fetch[0].req_secs?rsc_work_fetch[0].busy_time_estimator.get_busy_time():0
    );
    if (log_flags.work_fetch_debug) {
        char buf[256];
        request_string(buf);
        msg_printf(p, MSG_INFO, "%s", buf);
    }
}

// we just got a scheduler reply with the given jobs; update backoffs
//
void WORK_FETCH::handle_reply(
    PROJECT* p, SCHEDULER_REPLY*, vector<RESULT*> new_results
) {
    bool got_work[MAX_RSC];
    bool requested_work_rsc[MAX_RSC];
    for (int i=0; i<coprocs.n_rsc; i++) {
        got_work[i] = false;
        requested_work_rsc[i] = (rsc_work_fetch[i].req_secs > 0);
    }
    for (unsigned int i=0; i<new_results.size(); i++) {
        RESULT* rp = new_results[i];
        got_work[rp->avp->gpu_usage.rsc_type] = true;
    }

    for (int i=0; i<coprocs.n_rsc; i++) {
        // back off on a resource type if
        // - we asked for jobs
        // - we didn't get any
        // - we're not currently backed off for that type
        //   (i.e. don't back off because of a piggyback request)
        // - the RPC was done for a reason that is automatic
        //   and potentially frequent
        //
        if (requested_work_rsc[i] && !got_work[i]) {
            if (p->rsc_pwf[i].backoff_time < gstate.now) {
                switch (p->sched_rpc_pending) {
                case RPC_REASON_RESULTS_DUE:
                case RPC_REASON_NEED_WORK:
                case RPC_REASON_TRICKLE_UP:
                    p->rsc_pwf[i].resource_backoff(p, rsc_name_long(i));
                }
            }
        }
        // if we did get jobs, clear backoff
        //
        if (got_work[i]) {
            p->rsc_pwf[i].clear_backoff();
        }
    }
    p->pwf.request_if_idle_and_uploading = false;
}

// set up for initial RPC.
// arrange to always get one job, even if we don't need it or can't handle it.
// (this is probably what user wants)
//
void WORK_FETCH::set_initial_work_request(PROJECT* p) {
    for (int i=0; i<coprocs.n_rsc; i++) {
        rsc_work_fetch[i].req_secs = 1;
        if (i) {
            RSC_WORK_FETCH& rwf = rsc_work_fetch[i];
            if (rwf.ninstances ==  p->rsc_pwf[i].ncoprocs_excluded) {
                rsc_work_fetch[i].req_secs = 0;
            }
        }
        rsc_work_fetch[i].req_instances = 0;
        rsc_work_fetch[i].busy_time_estimator.reset();
    }
}

// called once, at client startup
//
void WORK_FETCH::init() {
    rsc_work_fetch[0].init(0, gstate.ncpus, 1);
    double cpu_flops = gstate.host_info.p_fpops;

    // use 20% as a rough estimate of GPU efficiency

    for (int i=1; i<coprocs.n_rsc; i++) {
        rsc_work_fetch[i].init(
            i, coprocs.coprocs[i].count,
            coprocs.coprocs[i].count*0.2*coprocs.coprocs[i].peak_flops/cpu_flops
        );
    }

    // see what resources anon platform projects can use
    //
    unsigned int i, j;
    for (i=0; i<gstate.projects.size(); i++) {
        PROJECT* p = gstate.projects[i];
        if (!p->anonymous_platform) continue;
        for (int k=0; k<coprocs.n_rsc; k++) {
            p->rsc_pwf[k].anon_skip = true;
        }
        for (j=0; j<gstate.app_versions.size(); j++) {
            APP_VERSION* avp = gstate.app_versions[j];
            if (avp->project != p) continue;
            p->rsc_pwf[avp->gpu_usage.rsc_type].anon_skip = false;
        }
    }
}

// clear backoff for app's resource
//
void WORK_FETCH::clear_backoffs(APP_VERSION& av) {
    av.project->rsc_pwf[av.gpu_usage.rsc_type].clear_backoff();
}

////////////////////////

void CLIENT_STATE::compute_nuploading_results() {
    unsigned int i;

    for (i=0; i<projects.size(); i++) {
        projects[i]->nuploading_results = 0;
        projects[i]->too_many_uploading_results = false;
    }
    for (i=0; i<results.size(); i++) {
        RESULT* rp = results[i];
        if (rp->state() == RESULT_FILES_UPLOADING) {
            rp->project->nuploading_results++;
        }
    }
    int n = gstate.ncpus;
    for (int j=1; j<coprocs.n_rsc; j++) {
        if (coprocs.coprocs[j].count > n) {
            n = coprocs.coprocs[j].count;
        }
    }
    n *= 2;
    for (i=0; i<projects.size(); i++) {
        if (projects[i]->nuploading_results > n) {
            projects[i]->too_many_uploading_results = true;
        }
    }
}

// Returns the estimated total elapsed time of this task.
// Compute this as a weighted average of estimates based on
// 1) the workunit's flops count (static estimate)
// 2) the current elapsed time and fraction done (dynamic estimate)
//
double ACTIVE_TASK::est_dur() {
    if (fraction_done >= 1) return elapsed_time;
    double wu_est = result->estimated_runtime();
    if (wu_est < elapsed_time) wu_est = elapsed_time;
    if (fraction_done <= 0) return wu_est;
    double frac_est = fraction_done_elapsed_time / fraction_done;

    // if app says fraction done is accurate, just use it
    //
    if (result->app->fraction_done_exact) return frac_est;

    // weighting of dynamic estimate is the fraction done
    // i.e. when fraction done is 0.5, weighting is 50/50
    //
    double fd_weight = fraction_done;
    double wu_weight = 1 - fd_weight;
    double x = fd_weight*frac_est + wu_weight*wu_est;
#if 0
    //if (log_flags.rr_simulation) {
        msg_printf(result->project, MSG_INFO,
            "[rr_sim] %s frac_est %f = %f/%f",
            result->name, frac_est, fraction_done_elapsed_time, fraction_done
        );
        msg_printf(result->project, MSG_INFO,
            "[rr_sim] %s dur: %.2f = %.3f*%.2f + %.3f*%.2f",
            result->name, x, fd_weight, frac_est, wu_weight, wu_est
        );
    //}
#endif
    return x;
}

// the fraction of time BOINC is processing
//
double CLIENT_STATE::overall_cpu_frac() {
    double x = time_stats.on_frac * time_stats.active_frac;
    if (x < 0.01) x = 0.01;
    if (x > 1) x = 1;
    return x;
}
double CLIENT_STATE::overall_gpu_frac() {
    double x = time_stats.on_frac * time_stats.gpu_active_frac;
    if (x < 0.01) x = 0.01;
    if (x > 1) x = 1;
    return x;
}
double CLIENT_STATE::overall_cpu_and_network_frac() {
    double x = time_stats.on_frac * time_stats.cpu_and_network_available_frac;
    if (x < 0.01) x = 0.01;
    if (x > 1) x = 1;
    return x;
}

// called when benchmarks change
//
void CLIENT_STATE::scale_duration_correction_factors(double factor) {
    if (factor <= 0) return;
    for (unsigned int i=0; i<projects.size(); i++) {
        PROJECT* p = projects[i];
        if (p->dont_use_dcf) continue;
        p->duration_correction_factor *= factor;
    }
    if (log_flags.dcf_debug) {
        msg_printf(NULL, MSG_INFO,
            "[dcf] scaling all duration correction factors by %f",
            factor
        );
    }
}

// Choose a new host CPID.
// If using account manager, do scheduler RPCs
// to all acct-mgr-attached projects to propagate the CPID
//
void CLIENT_STATE::generate_new_host_cpid() {
    host_info.generate_host_cpid();
    for (unsigned int i=0; i<projects.size(); i++) {
        if (projects[i]->attached_via_acct_mgr) {
            projects[i]->sched_rpc_pending = RPC_REASON_ACCT_MGR_REQ;
            projects[i]->set_min_rpc_time(now + 15, "Sending new host CPID");
        }
    }
}

inline const char* dont_fetch_string(int reason) {
    switch (reason) {
    case DONT_FETCH_GPUS_NOT_USABLE: return "GPUs not usable";
    case DONT_FETCH_PREFS: return "blocked by project preferences";
    case DONT_FETCH_CONFIG: return "client configuration";
    case DONT_FETCH_NO_APPS: return "no applications";
    case DONT_FETCH_AMS: return "account manager prefs";
    case DONT_FETCH_BACKOFF: return "backoff";
    case DONT_FETCH_ZERO_SHARE: return "zero resource share";
    case DONT_FETCH_BUFFER_FULL: return "job cache full";
    case DONT_FETCH_NOT_HIGHEST_PRIO: return "not highest priority project";
    }
    return "";
}

const char* cant_fetch_work_string(PROJECT* p, char* buf) {
    switch (p->pwf.cant_fetch_work_reason) {
    case CANT_FETCH_WORK_NON_CPU_INTENSIVE:
        return "non CPU intensive";
    case CANT_FETCH_WORK_SUSPENDED_VIA_GUI:
        return "suspended via Manager";
    case CANT_FETCH_WORK_MASTER_URL_FETCH_PENDING:
        return "master URL fetch pending";
    case CANT_FETCH_WORK_MIN_RPC_TIME:
        return "scheduler RPC backoff";
    case CANT_FETCH_WORK_DONT_REQUEST_MORE_WORK:
        return "\"no new tasks\" requested via Manager";
    case CANT_FETCH_WORK_DOWNLOAD_STALLED:
        return "some download is stalled";
    case CANT_FETCH_WORK_RESULT_SUSPENDED:
        return "some task is suspended via Manager";
    case CANT_FETCH_WORK_TOO_MANY_UPLOADS:
        return "too many uploads in progress";
    case CANT_FETCH_WORK_NOT_HIGHEST_PRIORITY:
        return "project is not highest priority";
    case CANT_FETCH_WORK_TOO_MANY_RUNNABLE:
        return "too many runnable tasks";
    case CANT_FETCH_WORK_DONT_NEED:
        if (coprocs.n_rsc == 1) {
            sprintf(buf, "don't need (%s)",
                dont_fetch_string(rsc_work_fetch[0].dont_fetch_reason)
            );
        } else {
            string x;
            x = "don't need (";
            for (int i=0; i<coprocs.n_rsc; i++) {
                char buf2[256];
                sprintf(buf2, "%s: %s",
                    rsc_name_long(i),
                    dont_fetch_string(rsc_work_fetch[i].dont_fetch_reason)
                );
                x += buf2;
                if (i < coprocs.n_rsc-1) {
                    x += "; ";
                }
            }
            x += ")";
            strcpy(buf, x.c_str());
        }
        return buf;
    }
    return "";
}
