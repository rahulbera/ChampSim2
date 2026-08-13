# Standalone: the tuner objective must build on the cluster with nothing but
# g++ and the two blbp headers. Deliberately outside the ChampSim build.
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -I../../inc
blbp_eval: blbp_eval.cc ../../inc/blbp/blbp.h
	$(CXX) $(CXXFLAGS) blbp_eval.cc -o $@
clean:
	$(RM) blbp_eval
.PHONY: clean
