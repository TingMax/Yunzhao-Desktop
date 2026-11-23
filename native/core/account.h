// One account for the desktop, the calendar and the table.
// Password login talks to https://yunzhao-desktop.com.
// wechat_qr and wecom_qr are reserved: the buttons call
// POST /api/auth/providers/<id>/start and wait for the server.
#pragma once

#include <windows.h>

namespace yunzhao {

// Shows the sign-in window until the server accepts this Windows user.
// Writes %LOCALAPPDATA%\Yunzhao\account.json so widget files stay under users\<id>.
bool AccountEnsure(HINSTANCE instance);

// After widgets.json is saved, send that user's copy to the server.
void AccountPublishLayout();

// Upload the current layout, drop the saved login, and open the sign-in window.
void AccountSignOut();

}
