/*	$NetBSD: msg_118.c,v 1.9 2025/09/06 20:18:41 rillig Exp $	*/
# 3 "msg_118.c"

/* Test for message: '%s' %s '%s' differs between traditional C and C90 [118] */

/* lint1-flags: -hw -X 351 */
/* lint1-only-if: lp64 */

int si;
unsigned ui;
long sl;
unsigned long ul;

void
test_shl(void)
{
	si <<= si;
	si <<= ui;
	/* expect+1: warning: 'int' <<= 'long' differs between traditional C and C90 [118] */
	si <<= sl;
	/* expect+1: warning: 'int' <<= 'unsigned long' differs between traditional C and C90 [118] */
	si <<= ul;

	si = si << si;
	si = si << ui;
	/* expect+1: warning: 'int' << 'long' differs between traditional C and C90 [118] */
	si = si << sl;
	/* expect+1: warning: 'int' << 'unsigned long' differs between traditional C and C90 [118] */
	si = si << ul;
}

void
test_shr(void)
{
	si >>= si;
	si >>= ui;
	si >>= sl;
	si >>= ul;

	si = si >> si;
	/* expect+1: warning: 'int' >> 'unsigned int' differs between traditional C and C90 [118] */
	si = si >> ui;
	/* expect+1: warning: 'int' >> 'long' differs between traditional C and C90 [118] */
	si = si >> sl;
	/* expect+1: warning: 'int' >> 'unsigned long' differs between traditional C and C90 [118] */
	si = si >> ul;
}
