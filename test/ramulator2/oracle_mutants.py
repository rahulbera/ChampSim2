"""Adapter mutants for the test 706 differential oracle.

Each mutant is a set of exact textual replacements in one source file, the
adapter src/ramulator2_memory_backend.cc unless `path` names another. Every
"old" text must occur exactly once in that file, so a mutant goes stale
visibly (test_tools.py checks all of them) instead of silently mutating
nothing when the source changes.

A mutant with `equivalent` set cannot change observable behavior; the reason
says why. The mutation runner expects it to survive and reports a detection of
it as a false positive. `min_cores` marks mutants that are equivalent below
that core count.
"""
from dataclasses import dataclass

ADAPTER = "src/ramulator2_memory_backend.cc"
DRIVER = "src/ramulator2_driver.cc"


@dataclass(frozen=True)
class Mutant:
    name: str
    description: str
    replacements: tuple
    equivalent: str = ""
    min_cores: int = 1
    path: str = ADAPTER


MUTANTS = (
    Mutant("M01-drop-partial-retry", "pop a head once any fragment is accepted, dropping its unaccepted suffix",
           (("const bool all_accepted = parent.accepted == parent.fragments;",
             "const bool all_accepted = parent.accepted == parent.fragments || parent.accepted > 0;"),)),
    Mutant("M02-resend-accepted-prefix", "resend a partial head's already accepted fragments",
           (("      while (parent.accepted < parent.fragments) {\n",
             "      if (parent.accepted > 0 && parent.accepted < parent.fragments) {\n        parent.accepted = 0;\n      }\n"
             "      while (parent.accepted < parent.fragments) {\n"),)),
    Mutant("M03-write-responses", "answer completed writes upstream",
           (("          ++counters.completed_writes;\n",
             "          ++counters.completed_writes;\n          if (parent.packet.response_requested) {\n"
             "            parent.returned->emplace_back(parent.packet);\n          }\n"),)),
    Mutant("M04-latency-from-enqueue", "start read latency when the parent is created, not at its first accepted fragment",
           (("        parents.emplace(id, parent_request{queue.requests->front(), queue.returned, queue.write, *block, fragments});\n",
             "        parents.emplace(id, parent_request{queue.requests->front(), queue.returned, queue.write, *block, fragments});\n"
             "        parents.at(id).first_accept = current_time;\n"),
            ("          parent.first_accept = current_time;\n", ""))),
    Mutant("M05-response-after-first-fragment", "answer a read when its first fragment completes",
           (("      if (parent.completed == parent.fragments) {\n",
             "      if (!parent.write && parent.completed == 1 && parent.packet.response_requested) {\n"
             "        parent.returned->emplace_back(parent.packet);\n      }\n      if (parent.completed == parent.fragments) {\n"),
            ("          if (parent.packet.response_requested) {\n            parent.returned->emplace_back(parent.packet);\n          }\n", ""))),
    Mutant("M06-queue-order-wq-first", "visit WQ before RQ and PQ",
           (("      queues.push_back({&channel->RQ, &channel->returned, false});\n      queues.push_back({&channel->PQ, &channel->returned, false});\n"
             "      queues.push_back({&channel->WQ, &channel->returned, true});\n",
             "      queues.push_back({&channel->WQ, &channel->returned, true});\n      queues.push_back({&channel->RQ, &channel->returned, false});\n"
             "      queues.push_back({&channel->PQ, &channel->returned, false});\n"),)),
    Mutant("M07-warmup-submits-retained-head", "let a later warmup continue a retained head natively",
           (("      if (warmup) {\n        break;\n      }\n", ""),)),
    Mutant("M08-ignore-response-suppression", "answer response-suppressed reads",
           (("          if (parent.packet.response_requested) {\n            parent.returned->emplace_back(parent.packet);\n          }\n",
             "          parent.returned->emplace_back(parent.packet);\n"),)),
    Mutant("M09-no-counter-reset", "keep event counters across begin_phase",
           (("    counters = {};\n", ""),)),
    Mutant("M10-gauge-counts-unadmitted-heads", "count a created but unaccepted head as outstanding",
           (("      if (parent.accepted != 0) {\n        ++result.outstanding_parents;", "      if (true) {\n        ++result.outstanding_parents;"),)),
    Mutant("M11-same-address-fragments", "send every fragment at the block address",
           (("const auto address = parent.block_address + parent.accepted * transaction_bytes;", "const auto address = parent.block_address;"),)),
    Mutant("M12-complete-before-pop", "flush completions before popping a fully accepted head",
           (("      const bool all_accepted = parent.accepted == parent.fragments;\n      if (all_accepted) {\n        queue.requests->pop_front();\n"
             "        queue.head.reset();\n      }\n      // In particular, pop the head before a synchronous last-write callback\n"
             "      // releases its context. Never use the parent reference after this flush.\n      progress += complete_requests();\n",
             "      const bool all_accepted = parent.accepted == parent.fragments;\n      progress += complete_requests();\n      if (all_accepted) {\n"
             "        queue.requests->pop_front();\n        queue.head.reset();\n      }\n"),),
           equivalent=("all_accepted is captured before the flush and the parent reference is not used after it; popping the feeder "
                       "head and resetting queue.head touch neither the parent map nor the mailbox, so responses, their order, "
                       "progress and counters are identical in either order. The comment's hazard (using the parent after a "
                       "synchronous last-write callback erased it) is not reachable from this reordering.")),
    Mutant("M13-warmup-answers-suppressed-reads", "answer response-suppressed reads that are popped without native submission",
           (("          if (!queue.write && queue.requests->front().response_requested) {\n", "          if (!queue.write) {\n"),)),
    Mutant("M14-rejections-not-counted-after-partial", "count a rejection only for a head with no accepted fragment",
           (("          ++counters.rejected_submissions;\n", "          if (parent.accepted == 0) {\n            ++counters.rejected_submissions;\n          }\n"),)),
    Mutant("M15-latency-origin-reset-at-phase", "restart live parents' latency origin at begin_phase",
           (("    counters = {};\n", "    counters = {};\n    for (auto& [pid, pr] : parents) {\n      pr.first_accept = current_time;\n    }\n"),)),
    Mutant("M16-idle-tick-counts-progress", "report the native tick itself as progress",
           (("    driver->tick();\n", "    driver->tick();\n    ++progress;\n"),)),
    Mutant("M17-roi-aliases-live", "return live statistics in place of the frozen ROI snapshot",
           (("    result.roi_ramulator2 = roi;\n", "    result.roi_ramulator2 = live_statistics();\n"),)),
    Mutant("M19-cpu-forced-zero", "submit every fragment as core 0",
           (("driver->send(parent.write, address, parent.packet.cpu, bytes, done)", "driver->send(parent.write, address, 0, bytes, done)"),),
           min_cores=2),
    # Out-of-range PREFETCH policy (89ba69f6).
    Mutant("M20-oor-prefetch-throws", "stop the run on an out-of-range prefetch, as for demand requests",
           (("      if (packet.type == access_type::PREFETCH) {\n        return std::nullopt;\n      }\n", ""),)),
    Mutant("M21-oor-prefetch-not-counted", "pop out-of-range prefetches without counting them",
           (("          if (!block) {\n            ++counters.out_of_range_prefetches;\n          }\n", ""),)),
    Mutant("M22-oor-prefetch-unanswered", "drop response-requested out-of-range prefetch reads without a response",
           (("          if (!queue.write && queue.requests->front().response_requested) {\n",
             "          if (!queue.write && queue.requests->front().response_requested && block) {\n"),)),
    Mutant("M23-oor-prefetch-uncounted-in-warmup", "count out-of-range prefetches only in measured phases",
           (("          if (!block) {\n            ++counters.out_of_range_prefetches;", "          if (!block && !warmup) {\n            ++counters.out_of_range_prefetches;"),)),
    Mutant("M24-oor-boundary-off-by-one", "treat the first block at capacity as in range and submit it",
           (("    if (block >= bytes || BLOCK_SIZE > bytes - block) {", "    if (block > bytes) {"),)),
    Mutant("M25-oor-answers-write-queue", "answer response-requested out-of-range prefetches that arrive through WQ",
           (("          if (!queue.write && queue.requests->front().response_requested) {\n",
             "          if ((!queue.write || !block) && queue.requests->front().response_requested) {\n"),)),
    Mutant("M26-oor-bypasses-fifo-order", "answer out-of-range prefetches waiting behind the queue head immediately",
           (("    long progress = 0;\n    while (!queue.requests->empty()) {\n",
             "    long progress = 0;\n    for (auto it = queue.requests->begin(); it != queue.requests->end();) {\n"
             "      if (it != queue.requests->begin() && !validate(*it)) {\n"
             "        if (!queue.write && it->response_requested) {\n          queue.returned->emplace_back(*it);\n        }\n"
             "        ++counters.out_of_range_prefetches;\n        ++progress;\n        it = queue.requests->erase(it);\n"
             "      } else {\n        ++it;\n      }\n    }\n    while (!queue.requests->empty()) {\n"),)),
    # The validation in front of that policy: invalid cores and out-of-range
    # non-PREFETCH requests stop the run in either phase.
    Mutant("M30-oor-demand-answered", "answer or drop out-of-range load, RFO, write and translation requests like prefetches",
           (("      if (packet.type == access_type::PREFETCH) {\n", "      if (packet.type != access_type::NUM_TYPES) {\n"),)),
    Mutant("M31-oor-prefetch-skips-core-check", "answer an out-of-range prefetch from an invalid core instead of stopping",
           (("    if (packet.cpu >= champsim::defs::num_cpus) {\n",
             "    if (packet.cpu >= champsim::defs::num_cpus\n"
             "        && !(packet.type == access_type::PREFETCH && packet.address.to<uint64_t>() >= static_cast<uint64_t>(capacity.count()))) {\n"),)),
    Mutant("M32-warmup-skips-validation", "pop invalid requests in warmup instead of stopping",
           (("        const auto block = validate(queue.requests->front());\n",
             "        std::optional<uint64_t> block;\n        try {\n          block = validate(queue.requests->front());\n"
             "        } catch (const std::runtime_error&) {\n          if (!warmup) {\n            throw;\n          }\n          block = 0;\n        }\n"),)),
    # Producer-pause recovery.
    Mutant("M27-stale-head-when-queue-empties", "keep a fully accepted head's id when its queue becomes empty",
           (("        queue.requests->pop_front();\n        queue.head.reset();\n",
             "        queue.requests->pop_front();\n        if (!queue.requests->empty()) {\n          queue.head.reset();\n        }\n"),)),
    Mutant("M28-driver-retains-native-callbacks", "keep a copy of every submitted callback in the native driver",
           (("  bool finalized_ = false;\n", "  bool finalized_ = false;\n  std::vector<std::function<void()>> retained_callbacks_;\n"),
            ("[done = std::move(done)](Ramulator::Request&) {", "[done = (retained_callbacks_.push_back(done), std::move(done))](Ramulator::Request&) {")),
           path=DRIVER),
    Mutant("M29-leak-suppressed-read-parents", "never release a completed response-suppressed read",
           (("        parents.erase(found);\n", "        if (parent.write || parent.packet.response_requested) {\n          parents.erase(found);\n        }\n"),)),
)


def apply(text, mutant):
    """Return the mutated text; raise ValueError naming a pattern that does not occur exactly once."""
    for old, new in mutant.replacements:
        count = text.count(old)
        if count != 1:
            raise ValueError(f"{mutant.name}: pattern occurs {count} times, expected once: {old!r}")
        text = text.replace(old, new)
    return text


def by_name(names):
    known = {mutant.name: mutant for mutant in MUTANTS}
    if not names:
        return list(MUTANTS)
    selected = []
    for name in names:
        matches = [mutant for key, mutant in known.items() if key == name or key.split("-", 1)[0] == name]
        if len(matches) != 1:
            raise KeyError(f"unknown mutant {name}")
        selected.append(matches[0])
    return selected
