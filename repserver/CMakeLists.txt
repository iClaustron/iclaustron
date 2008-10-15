add_executable(ic_repd ic_repserv.c)
target_link_libraries(ic_repd comm ic_util ic_port ic_apic ic_apid)
install (TARGETS is_repd DESTINATION bin)
