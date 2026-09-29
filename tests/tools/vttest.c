/* The console as modern command-line programs use it: it turns on virtual
 * terminal processing and writes escape sequences for colours, cursor moves,
 * erasing and the title, as Claude Code, node and PowerShell 7 do. What the
 * window shows must be coloured text and a drawn box, no escape characters. */
#include <windows.h>
#include <stdio.h>

static void out( HANDLE h, const wchar_t *s )
{
    DWORD written;
    WriteConsoleW( h, s, lstrlenW( s ), &written, NULL );
}

int main( void )
{
    HANDLE h = GetStdHandle( STD_OUTPUT_HANDLE );
    DWORD mode = 0;

    GetConsoleMode( h, &mode );
    if (!SetConsoleMode( h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING ))
        printf( "virtual terminal processing refused: %lu\n", GetLastError() );

    out( h, L"\x1b]0;vttest\x07" );
    out( h, L"\x1b[2J\x1b[H" );
    out( h, L"\x1b[1;31mred bold\x1b[0m \x1b[32mgreen\x1b[0m \x1b[34;47mblue on white\x1b[0m\n" );
    out( h, L"\x1b[38;5;208m256-colour orange\x1b[0m \x1b[38;2;255;0;255mtrue-colour magenta\x1b[0m\n" );
    out( h, L"\x1b[7minverse\x1b[27m normal\n" );
    out( h, L"┌────┐\n│ ok │\n└────┘\n" );
    out( h, L"this line is erased\r\x1b[2Kerased line replaced\n" );
    out( h, L"\x1b[5;40Hat row 5, column 40" );
    out( h, L"\x1b[10;1Hcursor up test\x1b[1Aabove\n\n" );
    out( h, L"\x1b[?25lcursor hidden, then shown\x1b[?25h\n" );
    Sleep( 60000 );
    return 0;
}
