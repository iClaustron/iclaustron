add_executable(ic_fsd ic_fileserv.c)
target_link_libraries(ic_fsd comm ic_util ic_apic ic_apid)
