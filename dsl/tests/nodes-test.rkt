#lang racket/base
;; Nodes' own push constants and invocations, replacing their material's, and loops of nodes,
;; repeated over ranges and data inputs when the graph is built

(require racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (header outputs)
  (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) ".zgal.h")) o)))

;; Lines of text without #line directives, trimmed
(define (code-lines text)
  (for/list ([l (in-list (string-split text "\n"))] #:unless (string-prefix? (string-trim l) "#line"))
    (string-trim l)))

(define materials
  (string-append
   "(struct Span (offset uint) (amount uint))\n"
   "(compute-material Fill (workgroup 64) (bindings (cells (buffer-array uint))) (push (offset uint) (amount uint))\n"
   "  (compute (when (< globalInvocationId.x amount) (write! (at cells (+ offset globalInvocationId.x)) 1))))\n"
   "(graphics-material Paint (push (row uint)) (outputs (color vec4)) (vertex (results #:position (vec4 0.0))) (fragment (vec4 (float row))))\n"))

(define (graph . items)
  (string-append materials "(graph G\n  (input spans (data Span))\n  (parameter n uint)\n  (buffer cells uint 64 #:frames shared)\n"
                 "  (image picture rgba8 (fixed 4 4) #:frames shared)\n  (target canvas (color picture #:load load))\n"
                 (string-join items "\n") ")\n"))

(test-case "Dispatches and draws push their own push constants after binding their material"
  (define h (header (compile-text
                     (graph "  (material filling Fill cells)"
                            "  (material painting Paint)"
                            "  (dispatch fill filling #:push ((offset 0) (amount n)) #:invocations (n))"
                            "  (draw paint canvas painting 3 #:push ((row (+ n 1))))"))))
  (check-true (string-contains? h "graph.add<zgal::OverridingDispatchNode>(\"fill\", material_filling, [=, material = material_filling](const RecordContext& record) { zgal::pushOverride(record, *material, [&](const zgal::FrameInfo& frame, Fill::Push& push) { push.offset = 0u; push.amount = in->n; }); }, [=](const zgal::FrameInfo& frame) { return Fill::workgroupsFor(in->n); }, std::nullopt)"))
  (check-true (string-contains? h "material->bind(record.commandBuffer, record.frameIndex); zgal::pushOverride(record, *material, [&](const zgal::FrameInfo& frame, Paint::Push& push) { push.row = (in->n + 1u); }); vkCmdDraw("))
  ;; The materials leave their push constants to the nodes
  (check-true (string-contains? h "[=](const zgal::FrameInfo& frame, Fill::Push& push) {\n")))

(test-case "Nodes give their own push constants and invocations"
  ;; A push constant the material leaves out, every node using it gives
  (check-zgal-error (graph "  (material filling Fill cells #:push ((offset 0)) #:invocations (1))"
                           "  (dispatch a filling #:push ((amount 1)))"
                           "  (dispatch b filling)")
                    'incomplete-material "doesn't give Fill's push constant amount" "(b doesn't)")
  (check-zgal-error (graph "  (material filling Fill cells #:push ((offset 0) (amount 1)))"
                           "  (dispatch a filling)")
                    'bad-option "needs them (#:invocations on the material or the dispatch)")
  (check-zgal-error (string-append "(import indirect)\n"
                                   (graph "  (buffer commands DispatchCommand 1 #:frames shared)"
                                          "  (material filling Fill cells #:push ((offset 0) (amount 1)))"
                                          "  (dispatch a filling #:indirect (at commands 0) #:invocations (1))"))
                    'bad-option "reads its workgroups from #:indirect, so it takes no #:invocations")
  (check-zgal-error (string-append "(import indirect)\n"
                                   (graph "  (buffer draws DrawCommand 1 #:frames shared)"
                                          "  (material painting Paint #:push ((row 0)))"
                                          "  (draw d canvas painting #:indirect (at draws 0) #:push ((row 1)))"))
                    'bad-option "#:push is for draws of a vertex count, and dispatches")
  (check-zgal-error (graph "  (material filling Fill cells #:push ((offset 0) (amount 1)) #:invocations (1))"
                           "  (dispatch a filling #:push ((count 1)))")
                    'unknown-name "material Fill has no push constant count"))

(test-case "Targets and materials with #:when are created only if it holds, and checked where they're used"
  (define text
    (string-append
     "(graphics-material Show (bindings (picture texture) (s sampler)) (outputs (color vec4))\n"
     "  (vertex (results #:position (vec4 0.0 0.0 0.0 1.0))) (fragment (sample picture s (vec2 0.5))))\n"
     "(graph G (parameter headless bool)\n"
     "  (image picture rgba8 (fixed 2 2) #:frames shared) (sampler s)\n"
     "  (target screen (surface #:depth none) #:when (not headless))\n"
     "  (material showing Show picture s #:when (not headless))\n"
     "  (draw show screen showing 3 #:when (not headless)))\n"))
  (define h (header (compile-text text)))
  (check-true (string-contains? h "std::optional<RenderTarget> target_screen;\n"))
  (check-true (string-contains? h "target_screen = built;"))
  (check-true (string-contains? h "std::optional<Handle<zgal::Instance<Show>>> material_showing;"))
  (check-true (string-contains? h "material_showing = context.resources().create<zgal::Instance<Show>>("))
  (check-true (string-contains? h "graph.formats(zgal::require(target_screen, \"G\", \"screen\"))"))
  (check-true (string-contains? h "[target = zgal::require(target_screen, \"G\", \"screen\"), material = zgal::require(material_showing, \"G\", \"showing\")]"))
  (check-zgal-error (string-replace text "#:depth none) #:when" "#:depth none) #:if") 'bad-option "#:if"))

(test-case "Loops repeat their nodes in C++ loops, in order, with an id per iteration"
  (define h (header (compile-text
                     (graph "  (material filling Fill cells)"
                            "  (material painting Paint)"
                            "  (dispatch first filling #:push ((offset 0) (amount 1)) #:invocations (1))"
                            "  (for ([s spans] [i (range 0 2)])"
                            "    (dispatch fill filling #:push ((offset (+ s.offset (uint i))) (amount s.amount)) #:invocations (s.amount) #:when (> s.amount 0))"
                            "    (readback snapshot cells))"
                            "  (for ([r (range (uint 0) n 2)])"
                            "    (for ([j (range r (+ r 2))])"
                            "      (draw paint canvas painting 3 #:push ((row j)))))"
                            "  (dispatch last filling #:push ((offset (.offset (at spans 0))) (amount 1)) #:invocations (1))"))))
  (define lines (code-lines h))
  (define (index-of-line text)
    (for/first ([l (in-list lines)] [i (in-naturals)] #:when (string-prefix? l text)) i))
  ;; Each iteration's nodes in order, between the nodes around the loops
  (define order
    (map index-of-line
         (list "const NodeId node = graph.add<zgal::OverridingDispatchNode>(\"first\""
               "for (const auto& loop_s : in->spans) {"
               "for (int32_t loop_i = 0; loop_i < 2; loop_i += 1) {"
               "const NodeId node = graph.add<zgal::OverridingDispatchNode>(std::string(\"fill \") + std::to_string(result.nodes.fill.size())"
               "const NodeId node = graph.add<BufferReadbackNode>(std::string(\"snapshot \")"
               "for (uint32_t loop_r = static_cast<uint32_t>(0); loop_r < in->n; loop_r += 2u) {"
               "for (uint32_t loop_j = loop_r; loop_j < (loop_r + 2u); loop_j += 1u) {"
               "const NodeId node = graph.addGraphics(std::string(\"paint \")"
               "const NodeId node = graph.add<zgal::OverridingDispatchNode>(\"last\"")))
  (check-false (memq #f order) (format "missing lines in ~a" order))
  (when (not (memq #f order))
    (check-equal? order (sort order <)))
  ;; Fields, elements and push constants of the node
  (check-true (string-contains? h "push.offset = (loop_s.offset + static_cast<uint32_t>(loop_i)); push.amount = loop_s.amount;"))
  (check-true (string-contains? h "zgal::elementOf(in->spans, 0, \"G\", \"spans\").offset"))
  (check-true (string-contains? h "return Fill::workgroupsFor(loop_s.amount);"))
  (check-true (string-contains? h "result.nodes.fill.push_back(node);"))
  (check-true (string-contains? h "std::vector<NodeId> fill;"))
  (check-true (string-contains? h "NodeId first;"))
  ;; The draw pushes its row after binding its material
  (check-true (string-contains? h "material->bind(record.commandBuffer, record.frameIndex); zgal::pushOverride(record, *material, [&](const zgal::FrameInfo& frame, Paint::Push& push) { push.row = loop_j; }); vkCmdDraw("))
  ;; A readback per iteration, by index
  (check-true (string-contains? h "snapshotContents(RenderGraph& graph, size_t index) const { return graph.nodeAs<BufferReadbackNode>(zgal::nodeAt(nodes.snapshot, index, \"G\", \"snapshot\"))"))
  ;; The materials leave their push constants to the nodes
  (check-true (string-contains? h "[=](const zgal::FrameInfo& frame, Fill::Push& push) {\n")))

(test-case "Apps enable every iteration of a looped node"
  (define h (header (compile-text
                     (string-append
                      (graph "  (material filling Fill cells #:push ((offset 0) (amount 1)) #:invocations (1))"
                             "  (for ([i (range 0 3)]) (dispatch fill filling))")
                      "(app A (state (filling bool #t)) (run G (n 1) #:enabled ((fill state.filling))))\n"))))
  (check-true (string-contains? h "for (const NodeId node : ids0_->nodes.fill) { graph0_->setEnabled(node, enabled_fill()); }")))

(test-case "Loops check what they hold and go over"
  (check-zgal-error (graph "  (for ([i (range 0 2)]) (material filling Fill cells #:invocations (1)))") 'loop-body "a loop's body holds nodes and loops")
  (check-zgal-error (graph "  (buffer sums uint 64 #:frames shared)"
                           "  (for ([i (range 0 2)]) (inclusive-scan scan cells sums #:op add #:count 64))")
                    'loop-body "scan is in a loop, but inclusive-scan nodes aren't yet")
  (check-zgal-error (string-append "(import indirect)\n"
                                   (graph "  (buffer draws DrawCommand 4 #:frames shared)"
                                          "  (material painting Paint #:push ((row 0)))"
                                          "  (for ([i (range 0 2)]) (draw d canvas painting #:indirect (at draws 0) #:draws 4 #:count (at cells 0)))"))
                    'loop-body "draws with a #:count aren't yet")
  (check-zgal-error (graph "  (material filling Fill cells #:invocations (1))"
                           "  (for ([i (range 0 2)]) (dispatch fill filling #:push ((offset (uint i)) (amount 1))))"
                           "  (dispatch after filling #:push ((offset (uint i)) (amount 1)))")
                    'unknown-name "i isn't declared")
  (check-zgal-error (graph "  (for ([n (range 0 2)]) (readback r cells))") 'duplicate-name "n is already declared")
  (check-zgal-error (graph "  (input more (per-frame (data uint)))" "  (for ([x more]) (readback r cells))") 'type-mismatch "not a data input")
  (check-zgal-error (graph "  (input k (per-frame uint))" "  (for ([i (range 0 k)]) (readback r cells))") 'not-build-time "k is a per-frame input")
  (check-zgal-error (graph "  (for ([i (range 0.0 2.0)]) (readback r cells))") 'type-mismatch "a range is of ints or uints")
  (check-zgal-error (graph "  (for ([i (range 0 4 0)]) (readback r cells))") 'malformed "a range's step is positive")
  (check-zgal-error (graph "  (for ([s spans]) (readback r cells #:when (> s.size 0)))") 'unknown-name "Span has no field size")
  (check-zgal-error (graph "  (for ([s spans]) (readback r cells #:when (> s.offset.x 0)))") 'type-mismatch "x is a uint, which has no fields")
  (check-zgal-error (graph "  (for ([i (range 0 2)]) (readback r cells #:when (> (.offset (at cells i)) 0)))") 'type-mismatch "takes a data input, and cells isn't one"))

(test-case "Graph expressions take swizzles of vectors"
  (define h (header (compile-text
                     (graph "  (material filling Fill cells)"
                            "  (dispatch fill filling #:push ((offset (.y (+ (.xy (uvec3 n 2 3)) (.bg (uvec3 n 2 3))))) (amount (.z (uvec3 n 2 3)))) #:invocations (1))"))))
  (check-true (string-contains? h "push.offset = ([](const auto& v) { return glm::uvec2(v.x, v.y); }(glm::uvec3(in->n, 2u, 3u)) + [](const auto& v) { return glm::uvec2(v.z, v.y); }(glm::uvec3(in->n, 2u, 3u))).y;"))
  (check-true (string-contains? h "push.amount = glm::uvec3(in->n, 2u, 3u).z;"))
  (check-zgal-error (graph "  (readback r cells #:when (> (.x (.xz (uvec2 n 1))) 0))")
                    'unknown-name "xz isn't a swizzle of a uvec2"))

(test-case "Read events take readbacks outside loops"
  (check-zgal-error (string-append
                     (graph "  (for ([i (range 0 2)]) (readback r cells))")
                     "(app A (state (seen uint 0)) (on (read r xs) (with state #:seen (count xs))) (run G (n 1)))\n")
                    'type-mismatch "readback r is in a loop"))
