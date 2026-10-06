/* notes
DWORD
ptw32_RegisterCancelation (PAPCFUNC unused1, HANDLE threadH, DWORD unused2)
{
	CONTEXT context;
	context.ContextFlags = CONTEXT_CONTROL;
	GetThreadContext (threadH, &context);
	PTW32_PROGCTR (context) = (DWORD_PTR) ptw32_cancel_self;
	SetThreadContext (threadH, &context);
	return 0;
}
#define PTW32_PROGCTR(Context) ((Context).Iar)
*/
#include "kernelp.h"


