#lang racket/base
;; Nodes' own push constants and invocations, replacing their material's

(require racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (header outputs)
  (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) ".zgal.h")) o)))

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
