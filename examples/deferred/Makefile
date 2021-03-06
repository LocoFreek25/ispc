
ARCH = $(shell uname)

TASK_CXX=../tasksys.cpp
TASK_LIB=-lpthread
TASK_OBJ=$(addprefix objs/, $(subst ../,, $(TASK_CXX:.cpp=.o)))

CXX=g++
CXXFLAGS=-Iobjs/ -O3 -Wall -m64
ISPC=ispc
ISPCFLAGS=-O2 --target=sse2,sse4-x2,avx-x2 --arch=x86-64 --math-lib=fast

OBJS=objs/main.o objs/common.o objs/kernels_ispc.o objs/kernels_ispc_sse2.o \
	objs/kernels_ispc_sse4.o objs/kernels_ispc_avx.o \
	objs/dynamic_c.o objs/dynamic_cilk.o

default: deferred_shading

.PHONY: dirs clean
.PRECIOUS: objs/kernels_ispc.h

dirs:
	/bin/mkdir -p objs/

clean:
	/bin/rm -rf objs *~ deferred_shading

deferred_shading: dirs $(OBJS) $(TASK_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(TASK_OBJ) -lm $(TASK_LIB)

objs/%.o: %.cpp objs/kernels_ispc.h deferred.h
	$(CXX) $< $(CXXFLAGS) -c -o $@

objs/%.o: ../%.cpp
	$(CXX) $< $(CXXFLAGS) -c -o $@

objs/%_ispc.h objs/%_ispc.o objs/%_ispc_sse2.o objs/%_ispc_sse4.o objs/%_ispc_avx.o: %.ispc
	$(ISPC) $(ISPCFLAGS) $< -o objs/$*_ispc.o -h objs/$*_ispc.h
