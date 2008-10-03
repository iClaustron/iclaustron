add_executable (ic_cclient ic_cclient.c)
target_link_libraries (ic_cclient comm ic_util ic_port ic_apic)
