/* common.h — shared includes and small string helpers for the DOS door. */
#ifndef ANET_DOS_COMMON_H
#define ANET_DOS_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>

/* Platform shim: the door shares its main loop with the native Win32 local
 * client (anetmrc_l.exe). delay() and INT 28h are DOS-only; the Win32 build
 * substitutes Sleep() and a no-op idle hint. */
#if defined(__DOS__) || defined(__WATCOMC__)
#  include <dos.h>
#  define anet_delay(ms) delay(ms)
#  define anet_idle_hint() __asm { int 28h }
#elif defined(_WIN32)
#  include <windows.h>
#  define anet_delay(ms) Sleep((DWORD)(ms))
#  define anet_idle_hint() ((void)0)
#else
#  include <unistd.h>
#  define anet_delay(ms) usleep((useconds_t)(ms) * 1000)
#  define anet_idle_hint() ((void)0)
#endif

#define IRC_LINE_MAX 512
#define PROFILE_MAX 8
static void safe_copy(char *dst, size_t dstsz, const char *src){size_t i;if(!dst||dstsz==0)return;if(!src)src="";for(i=0;i+1<dstsz&&src[i];++i)dst[i]=src[i];dst[i]=0;}
static void trim_crlf(char *s){size_t n;if(!s)return;n=strlen(s);while(n&&(s[n-1]=='\r'||s[n-1]=='\n'))s[--n]=0;}
#endif
