#lang racket/base
;; GPU-driven work: the compute library's kernels (scans and reductions)

(require racket/list
         racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (named outputs suffix)
  (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) suffix)) o)))

(define (shader-names outputs)
  (sort (for/list ([o (in-list outputs)] #:when (regexp-match? #px"[.]comp$" (output-path o)))
          (regexp-replace #px"^.*/" (output-path o) ""))
        string<?))

(define library
  (string-append
   "(graph G (input n (per-frame uint))\n"
   "  (buffer values uint 1000 #:frames shared)\n"
   "  (buffer sums uint 1000 #:frames shared)\n"
   "  (buffer floats float 1000 #:frames shared)\n"
   "  (buffer total float 4 #:frames shared)\n"
   "  (buffer counts uint 2 #:frames shared)\n"
   "  (inclusive-scan scan values sums #:op add #:count n)\n"
   "  (exclusive-scan before values sums #:op max #:count (at counts 1))\n"
   "  (reduce smallest floats (at total 2) #:op min #:count 1000))\n"))

(test-case "Kernel nodes instantiate the library's materials for their types, operators and counts"
  (define outputs (compile-text library))
  ;; Scans of the same type and operator would share them; inclusive and exclusive scans differ in
  ;; their last step only
  (check-equal? (shader-names outputs)
                '("ZgalReduceFinishFloatMin.comp" "ZgalReduceFinishUintAdd.comp" "ZgalReduceFinishUintMaxCounted.comp"
                  "ZgalScanApplyUintAddInclusive.comp" "ZgalScanApplyUintMaxCountedExclusive.comp"
                  "ZgalScanPartialsFloatMin.comp" "ZgalScanPartialsUintAdd.comp" "ZgalScanPartialsUintMaxCounted.comp"
                  "ZgalScanReduceFloatMin.comp" "ZgalScanReduceUintAdd.comp" "ZgalScanReduceUintMaxCounted.comp"))
  (define h (named outputs ".zgal.h"))
  ;; One node dispatching the steps, which the graph synchronizes as a whole
  (check-true (string-contains? h "graph.add<DispatchSequenceNode>(\"scan\", std::vector<DispatchSequenceNode::Step>{{material_zgal_scan_reduce}, {material_zgal_scan_scanPartials}, {material_zgal_scan_apply}})"))
  (check-true (string-contains? h "graph.add<DispatchSequenceNode>(\"smallest\", std::vector<DispatchSequenceNode::Step>{{material_zgal_smallest_reduce}, {material_zgal_smallest_finish}})"))
  ;; Workgroups for the count's tiles of 256, at most 256 of them; all 256 for a count on the GPU
  (check-true (string-contains? h "ZgalScanReduceUintAdd::workgroupsFor((std::min(std::max(((in->n(frame) + 255u) / 256u), 1u), 256u) * 256u))"))
  (check-true (string-contains? h "ZgalScanReduceUintMaxCounted::workgroupsFor((256u * 256u))"))
  (check-true (string-contains? h ".counts = graph.storageBufferEntry(buffer_counts)"))
  ;; Their scratch buffers aren't the graph's ids
  (check-true (string-contains? h "graph.createBuffer(\"zgal_scan_partials\", FrameMode::PerFrame, VkDeviceSize(256u) * sizeof(uint32_t))"))
  (check-false (string-contains? h "BufferId zgal_scan_partials;"))
  ;; Minima and maxima scan with shuffles (Metal scans only sums); sums natively
  (check-true (string-contains? (named outputs "ZgalScanApplyUintMaxCountedExclusive.comp") "subgroupShuffle"))
  (check-true (string-contains? (named outputs "ZgalScanApplyUintAddInclusive.comp") "subgroupInclusiveAdd"))
  ;; The float minimum's identity is infinity
  (check-true (string-contains? (named outputs "ZgalScanReduceFloatMin.comp") "uintBitsToFloat(uint(2139095040))")))

(test-case "Kernel nodes check their buffers, operator and count"
  (check-zgal-error (string-replace library "(buffer floats float" "(buffer floats vec2") 'type-mismatch
                    "reduce combines uints, ints or floats, but floats holds vec2")
  (check-zgal-error (string-replace library "(at total 2)" "(at sums 2)") 'type-mismatch
                    "reduce's destination is an element of a buffer of floats, but sums holds uint")
  (check-zgal-error (string-replace library "(at total 2)" "total") 'malformed
                    "reduce's destination is a buffer's element, (at buffer index), not total")
  (check-zgal-error (string-replace library "values sums #:op add" "values floats #:op add") 'type-mismatch
                    "values holds uint, but floats holds float")
  (check-zgal-error (string-replace library "#:op add #:count n" "#:count n") 'bad-option
                    "inclusive-scan needs #:op (add, min, max)")
  (check-zgal-error (string-replace library "#:op add #:count n" "#:op add") 'bad-option
                    "inclusive-scan needs #:count")
  (check-zgal-error (string-replace library "(at counts 1)" "(at floats 1)") 'type-mismatch
                    "exclusive-scan's #:count is an element of a buffer of uints, but floats holds float")
  (check-zgal-error (string-replace library "#:count n" "#:count 1.5") 'type-mismatch "inclusive-scan's #:count")
  (check-zgal-error (string-replace library "(inclusive-scan scan values sums" "(inclusive-scan scan values")
                    'malformed "expected (inclusive-scan name source destination #:op op #:count n)"))

(test-case "Barriers may depend on what the shader reads from buffers it doesn't write"
  (define counted
    (string-append
     "(compute-material C (workgroup 64) (bindings (counts (buffer-array uint)) (results (buffer-array uint)))\n"
     "  (shared (s (array uint 64)))\n"
     "  (compute (for ([t (range 0 (at counts 0))]) (write! (at s localInvocationId.x) t) (barrier!) (write! (at results t) (at s 0)))))\n"))
  (check-not-exn (λ () (compile-text counted)))
  ;; But not on what it writes, which can change during the dispatch
  (check-zgal-error (string-replace counted "(write! (at results t) (at s 0))" "(write! (at counts 1) (at s 0))")
                    'bad-barrier "in a loop whose range can differ between invocations"))

(define indirect-program
  (string-append
   "(import indirect)\n"
   "(compute-material Count (workgroup 1) (bindings (tally (buffer-array uint))) (compute (atomic-add! (at tally 0) 1)))\n"
   "(graphics-material Paint (outputs (color vec4)) (vertex (results #:position (vec4 0.0))) (fragment (vec4 1.0)))\n"
   "(graph G\n"
   "  (buffer dispatches DispatchCommand 2 #:frames shared)\n"
   "  (buffer draws DrawCommand 8 #:frames shared)\n"
   "  (buffer counts uint 1 #:frames shared)\n"
   "  (buffer tally uint 1 #:frames shared)\n"
   "  (image picture rgba8 (fixed 4 4) #:frames shared)\n"
   "  (target canvas (color picture))\n"
   "  (material counting Count tally)\n"
   "  (material painting Paint)\n"
   "  (dispatch count counting #:indirect (at dispatches 1))\n"
   "  (draw paint canvas painting #:indirect (at draws 2) #:draws 4 #:count (at counts 0)))\n"))

(test-case "Dispatches and draws read commands shaders wrote"
  (define outputs (compile-text indirect-program))
  (define h (named outputs ".zgal.h"))
  (check-true (string-contains? h "graph.add<DispatchNode>(\"count\", material_counting, BufferOffset{buffer_dispatches, VkDeviceSize(1u) * sizeof(DispatchCommand)})"))
  ;; A count draws natively where the device can, else after a node limiting the commands
  (check-true (string-contains? h "zgal::addCountedDraw(graph, \"paint\", IndirectDrawNode::Desc{.material = material_painting, .target = target_canvas, .commands = BufferOffset{buffer_draws, VkDeviceSize(2u) * sizeof(DrawCommand)}, .maxDraws = 4u}, BufferOffset{buffer_counts, VkDeviceSize(0u) * sizeof(uint32_t)}, material_zgal_paint_limit, buffer_zgal_paint_commands)"))
  (check-true (string-contains? h "features.core.features.multiDrawIndirect = VK_TRUE;"))
  (check-true (for/or ([o (in-list outputs)]) (string-suffix? (output-path o) "ZgalLimitDrawsDraw.comp")))
  ;; Without a count, one command by default
  (define single (named (compile-text (string-replace indirect-program " #:draws 4 #:count (at counts 0)" "")) ".zgal.h"))
  (check-true (string-contains? single "graph.add<IndirectDrawNode>(\"paint\", IndirectDrawNode::Desc{.material = material_painting, .target = target_canvas, .commands = BufferOffset{buffer_draws, VkDeviceSize(2u) * sizeof(DrawCommand)}, .maxDraws = 1u})"))
  (check-false (string-contains? single "multiDrawIndirect")))

(test-case "Indirect dispatches and draws check their commands"
  (check-zgal-error (string-replace indirect-program " #:indirect (at dispatches 1)" "") 'bad-option
                    "material counting has no #:invocations, so dispatch count needs them, or dispatches it #:indirect")
  (check-zgal-error (string-replace indirect-program "(at dispatches 1)" "(at draws 1)") 'type-mismatch
                    "#:indirect reads DispatchCommands, (import indirect), but draws holds DrawCommand")
  (check-zgal-error (string-replace indirect-program "(at dispatches 1)" "dispatches") 'malformed
                    "#:indirect is the first command's place in a buffer, (at commands index)")
  (check-zgal-error (string-replace indirect-program "(at counts 0)))" "(at draws 0)))") 'type-mismatch
                    "a draw's #:count is an element of a buffer of uints, but draws holds DrawCommand")
  (check-zgal-error (string-replace indirect-program "#:indirect (at draws 2) #:draws 4 #:count (at counts 0)" "3 #:draws 4") 'bad-option
                    "#:draws is for draws with #:indirect")
  (check-zgal-error (string-replace indirect-program "(buffer dispatches DispatchCommand 2 #:frames shared)" "(import-buffer dispatches DispatchCommand)")
                    'type-mismatch "dispatches is an imported buffer, not a graph buffer"))
