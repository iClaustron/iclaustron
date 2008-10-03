add_executable(test_comm test_comm.c)
target_link_libraries(test_comm comm ic_util ic_port ic_apic ic_apid)
