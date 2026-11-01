#	$NetBSD$

PROG=	aprefd
SRCS=	aprefd.c aprefio.c
MAN=	aprefd.8

LDADD+=	-lutil -lz
DPADD+=	${LIBUTIL} ${LIBZ}

.include <bsd.prog.mk>
