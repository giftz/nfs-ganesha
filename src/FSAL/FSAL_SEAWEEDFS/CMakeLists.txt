# SPDX-License-Identifier: LGPL-3.0-or-later

########### SeaweedFS FSAL ###############

set(seaweedfs_LIB_SRCS
   main.c
   config.c
   connection.c
   handle.c
)

if(USE_FSAL_SEAWEEDFS)
   add_library(fsalseaweedfs SHARED ${seaweedfs_LIB_SRCS})
   add_sanitizers(fsalseaweedfs)

   target_link_libraries(fsalseaweedfs
     ganesha_nfsd
     ${SYSTEM_LIBRARIES}
     ${OPENSSL_LIBRARIES}
   )

   set_target_properties(fsalseaweedfs PROPERTIES 
     VERSION 4.3.0 
     SOVERSION 4
     OUTPUT_NAME "fsalseaweedfs"
   )

   install(TARGETS fsalseaweedfs COMPONENT fsal
           LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ganesha
   )

endif(USE_FSAL_SEAWEEDFS)

########### install files ###############