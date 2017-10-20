dnl Configury specific to the libfabric shm provider

dnl Called to configure this provider
dnl
dnl Arguments:
dnl
dnl $1: action if configured successfully
dnl $2: action if not configured successfully
dnl
AC_DEFUN([FI_SHM_CONFIGURE],[
	# Determine if we can support the shm provider
	shm_happy=0
	AS_IF([test x"$enable_shm" != x"no"],
	      [
	       # check if shm_open and CMA support are present
	       AC_CHECK_FUNCS(shm_open,
			      process_vm_readv,
			     [shm_happy=1],
			     [shm_happy=0])

	       # look for shm_open in librt if not already present
	       AS_IF([test $shm_happy -eq 0],
		     [FI_CHECK_PACKAGE([shm_lib],
				[sys/mman.h],
				[rt],
				[shm_open],
				[],
				[],
				[],
				[shm_happy=1],
				[shm_happy=0])])
	      ])

	AS_IF([test $shm_happy -eq 1], [$1], [$2])
])
