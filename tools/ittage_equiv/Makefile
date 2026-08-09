# Standalone: deliberately outside the ChampSim build. It links BOTH the
# pristine cbp2025 reference and our parameterised copy, whose macros and class
# names collide -- hence one translation unit each.
#
#   make && ./ittage_equiv <stream.bin>
#
# Generate a stream with:
#   python3 extract_stream.py <trace.champsim2.zst> stream.bin <n_records>

CXXFLAGS := -std=c++17 -O2 -w -I../../inc -I.

ittage_equiv: pristine.cc parameterized.cc main.cc api.h
	$(CXX) $(CXXFLAGS) pristine.cc parameterized.cc main.cc -o $@

clean:
	$(RM) ittage_equiv
.PHONY: clean
