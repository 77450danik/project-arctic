/*
 * The Linux gate, as a Windows program asks it (docs/linux-gate.md)
 *
 * The gate's own settings go through the same service every wsl.exe command
 * goes through, host/init/arctic-lxss.c, with the request REQ_GATE: reading
 * the state needs no window, changing it always puts up the consent window
 * with the password that is set now, as changing the settings of User Account
 * Control does in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTIC_GATE_CLIENT_H
#define __ARCTIC_GATE_CLIENT_H

enum gate_result
{
    GATE_OK,       /* done; what the gate said is in the caller's buffer */
    GATE_DENIED,   /* the consent window was answered No, or the password was wrong */
    GATE_ERROR     /* the subsystem is not running, or it refused the request */
};

/* reads whether the password is required */
enum gate_result gate_password_required( BOOL *required );
/* both ask the user: the consent window with the current password */
enum gate_result gate_set_password( const WCHAR *password );
enum gate_result gate_require_password( BOOL required );

#endif
