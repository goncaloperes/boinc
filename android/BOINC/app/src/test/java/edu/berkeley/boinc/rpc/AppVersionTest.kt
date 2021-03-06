/*
 * This file is part of BOINC.
 * http://boinc.berkeley.edu
 * Copyright (C) 2020 University of California
 *
 * BOINC is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * BOINC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with BOINC.  If not, see <http://www.gnu.org/licenses/>.
 */
package edu.berkeley.boinc.rpc

import com.google.common.testing.EqualsTester
import org.junit.jupiter.api.Test

class AppVersionTest {
    @Test
    fun `Test equals() and hashCode()`() {
        EqualsTester().addEqualityGroup(AppVersion(), AppVersion())
                .addEqualityGroup(AppVersion("App Name"))
                .addEqualityGroup(AppVersion(versionNum = 1))
                .addEqualityGroup(AppVersion(platform = "Platform"))
                .addEqualityGroup(AppVersion(planClass = "Plan class"))
                .addEqualityGroup(AppVersion(apiVersion = "API version"))
                .addEqualityGroup(AppVersion(avgNoOfCPUs = 1.5))
                .addEqualityGroup(AppVersion(maxNoOfCPUs = 15.5))
                .addEqualityGroup(AppVersion(gpuRam = 2.0))
                .addEqualityGroup(AppVersion(app = App()))
                .addEqualityGroup(AppVersion(project = Project()))
                .testEquals()
    }
}
