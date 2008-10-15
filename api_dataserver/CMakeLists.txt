add_library (ic_apid SHARED ic_apid.c)
target_link_libraries (ic_apid comm ic_port ic_apic)
install (TARGETS ic_apid
         RUNTIME DESTINATION bin
         LIBRARY DESTINATION lib
         ARCHIVE DESTINATION lib/iClaustron)
