#ifndef QUOTA_FS_H
#define QUOTA_FS_H

#if defined (HAVE_STRUCT_DQBLK_CURBLOCKS) || \
	defined (HAVE_STRUCT_DQBLK_CURSPACE)
#  define HAVE_FS_QUOTA
#endif

#ifdef HAVE_SYS_QUOTA_H
#  include <sys/quota.h> /* Linux, HP-UX */
#elif defined(HAVE_SYS_FS_UFS_QUOTA_H)
#  include <sys/fs/ufs_quota.h> /* Solaris */
#elif defined(HAVE_UFS_UFS_QUOTA_H)
#  include <ufs/ufs/quota.h> /* BSDs */
#elif defined(HAVE_JFS_QUOTA_H)
#  include <jfs/quota.h> /* AIX */
#else
#  undef HAVE_FS_QUOTA
#endif

#ifdef HAVE_QUOTACTL
#  ifdef HAVE_SYS_QUOTA_H
#    ifdef QCMD
#      define FS_QUOTA_LINUX
#    else
#      define FS_QUOTA_HPUX
#    endif
#  else
#    define FS_QUOTA_BSDAIX
#  endif
#elif defined (HAVE_Q_QUOTACTL)
#  define FS_QUOTA_SOLARIS
#else
#  undef HAVE_FS_QUOTA
#endif

#endif
