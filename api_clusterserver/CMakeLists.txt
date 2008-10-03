add_library (ic_apic STATIC ic_apic.c)
target_link_libraries (ic_apic ic_port comm)
