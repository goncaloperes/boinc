// $Id$
//
// The contents of this file are subject to the BOINC Public License
// Version 1.0 (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://boinc.berkeley.edu/license_1.0.txt
// 
// Software distributed under the License is distributed on an "AS IS"
// basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
// License for the specific language governing rights and limitations
// under the License. 
// 
// The Original Code is the Berkeley Open Infrastructure for Network Computing. 
// 
// The Initial Developer of the Original Code is the SETI@home project.
// Portions created by the SETI@home project are Copyright (C) 2002
// University of California at Berkeley. All Rights Reserved. 
// 
// Contributor(s):
//
// Revision History:
//
// $Log$
// Revision 1.1  2004/05/17 22:15:10  rwalton
// *** empty log message ***
//
//

#if defined(__GNUG__) && !defined(__APPLE__)
#pragma implementation "URLValidator.h"
#endif

#include "stdwx.h"
#include "URLValidator.h"


IMPLEMENT_DYNAMIC_CLASS(CURLValidator, wxValidator)


CURLValidator::CURLValidator(void) :
    wxValidator()
{
    SetBellOnError(false);

    // Expression From:
    // http://msdn.microsoft.com/library/default.asp?
    //   url=/library/en-us/script56/html/reconbackreferences.asp
    m_reURL.Compile("/(\\w+):\\/\\/([^/:]+)(:\\d*)?([^# ]*)/");
}


CURLValidator::~CURLValidator(void)
{
}


bool CURLValidator::Validate(wxWindow* parent)
{


    return true;
}



