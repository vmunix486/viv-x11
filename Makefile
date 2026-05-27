# You should probably change these variables. Especially LIBPATH
CXX = c++
CXXFLAGS = -O3
LIBS = -lX11 -lXt -lXaw
LIBPATH = -L/usr/X11/lib64
STATICFLAGS = -static -static-libgcc

all:
	$(CXX) $(CXXFLAGS) $(LIBS) $(LIBPATH) src/main.cpp -o viv

static:
	@echo "Warning: does not work"
	$(CXX) $(CXXFLAGS) $(LIBS) $(LIBPATH) $(STATICFLAGS) src/main.cpp -o viv

clean:
	rm -f viv
