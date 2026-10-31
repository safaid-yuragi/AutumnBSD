#	$NetBSD$

PROG=	aprefctl
SRCS=	aprefctl.c aprefio.c
MAN=	aprefctl.8

.PATH:	${.CURDIR}/../aprefd
CPPFLAGS+=	-I${.CURDIR}/../aprefd

LDADD+=	-lutil -lz
DPADD+=	${LIBUTIL} ${LIBZ}

.include <bsd.prog.mk>
