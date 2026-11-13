/*
 * bcdboot.exe
 *
 * Windows always has it, and programs take that for granted: Rufus reads its
 * version when it starts and has no way to go on without one. Arctic starts
 * from its own loader and has no BCD store to make, so the program exists,
 * has a version and answers as it does when it cannot copy the boot files.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <windows.h>

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    fputs( "Failure when attempting to copy boot files.\n", stderr );
    return 1;
}
