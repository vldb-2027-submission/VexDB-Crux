#
# Copyright (c) 2021 Huawei Technologies Co.,Ltd.
# 
# openGauss is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
# 
#          http://license.coscl.org.cn/MulanPSL2
# 
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
# ---------------------------------------------------------------------------------------
# 
# Makefile
# 
# 
# IDENTIFICATION
#        src/bin/gs_loader/Makefile
# 
# ---------------------------------------------------------------------------------------
top_builddir=../../..
include $(top_builddir)/src/Makefile.global
subdir=src/bin/gs_loader

all: vex_loader
vex_loader: 
	g++ gs_loader.cpp -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) vex_loader.sh$(X) '$(DESTDIR)$(bindir)/vex_loader.sh$(X)'
	$(INSTALL_PROGRAM) vex_loader$(X) '$(DESTDIR)$(bindir)/vex_loader$(X)'

uninstall:
	rm -f  '$(DESTDIR)$(bindir)/vex_loader.sh$(X)'
	rm -f  '$(DESTDIR)$(bindir)/vex_loader$(X)'
