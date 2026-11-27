# Host build of the CPU micro-benchmark: make -C port/switch1/probe/linux && ./cpubench
cpubench: cpubench_main.cpp ../source/cpubench.h
	$(CXX) -O2 -std=c++17 -o $@ cpubench_main.cpp
clean:
	rm -f cpubench
