add_library (ic_apid STATIC ic_apid.c)
target_link_libraries (ic_apid comm ic_port ic_apic)
