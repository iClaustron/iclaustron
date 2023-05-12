## Start with an empty file

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = port util comm protocol api include cluster_server bootstrap scripts \
          cfg pcntrl cluster_client cluster_mgr repserver fileserver test

EXTRA_DIST = bootstrap.sh count_line.sh setup.sh

dist-hook:
	mkdir $(distdir)/examples
	cp -p $(srcdir)/examples/config.ini $(distdir)/examples
	cp -p $(srcdir)/examples/grid_common.ini $(distdir)/examples
	cp -p $(srcdir)/examples/kalle.ini $(distdir)/examples
	cp -p $(srcdir)/examples/jocke.ini $(distdir)/examples
	mkdir $(distdir)/doc
	cp -p $(srcdir)/NEWS $(distdir)
	cp -p $(srcdir)/COPYING $(distdir)
	cp -p $(srcdir)/INSTALL $(distdir)
	cp -p $(srcdir)/README $(distdir)
	cp -p $(srcdir)/README-CONFIG $(distdir)
	cp -p $(srcdir)/doc/config_parameters.txt $(distdir)/doc
	cp -p $(srcdir)/doc/bootstrap.txt $(distdir)/doc
	cp -p $(srcdir)/doc/iclaustron_grid.txt $(distdir)/doc

install-data-hook:
	mkdir -p $(prefix)/examples
	cp -p $(srcdir)/examples/config.ini $(prefix)/examples
	cp -p $(srcdir)/examples/grid_common.ini $(prefix)/examples
	cp -p $(srcdir)/examples/kalle.ini $(prefix)/examples
	cp -p $(srcdir)/examples/jocke.ini $(prefix)/examples
	mkdir -p $(prefix)/doc
	cp -p $(srcdir)/README $(prefix)/doc
	cp -p $(srcdir)/README-CONFIG $(prefix)/doc
	cp -p $(srcdir)/AUTHORS $(prefix)/doc
	cp -p $(srcdir)/NEWS $(prefix)/doc
	cp -p $(srcdir)/COPYING $(prefix)/doc
	cp -p $(srcdir)/INSTALL $(prefix)/doc
	cp -p $(srcdir)/ChangeLog $(prefix)/doc
	cp -p $(srcdir)/doc/config_parameters.txt $(prefix)/doc
	cp -p $(srcdir)/doc/bootstrap.txt $(prefix)/doc
	cp -p $(srcdir)/doc/iclaustron_grid.txt $(prefix)/doc

