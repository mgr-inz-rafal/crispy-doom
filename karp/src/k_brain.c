//
// DESCRIPTION:
//	Karp priority system. Stage 1 so far: the bot only idles.
//

#include "karp.h"
#include "k_brain.h"

const char *K_Brain_Status(void)
{
    if (!K_AutoplayEnabled())
    {
        return "VIEWER - run with -autoplay to let the bot drive";
    }

    return "IDLE";
}
