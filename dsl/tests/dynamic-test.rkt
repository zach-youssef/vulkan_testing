#lang racket/base
;; Dynamic data: buffers sized each frame, and array bindings

(require racket/list
         racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (named outputs suffix)
  (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) suffix)) o)))

(test-case "Buffers sized from per-frame inputs are recreated when their size changes"
  (define h (named (compile-text (string-append
                                  "(graph G (input n (per-frame uint)) (parameter m uint)\n"
                                  "  (buffer grown float (* n 2) #:frames per-frame)\n"
                                  "  (buffer steady float (+ m 1) #:frames shared))"))
                   ".zgal.h"))
  (check-true (string-contains? h "graph.createBuffer(\"grown\", FrameMode::PerFrame, [=](const FrameContext& frameContext) { const zgal::FrameInfo frame = zgal::frameInfo(frameContext); return VkDeviceSize((in->n(frame) * 2u)) * sizeof(float); })"))
  (check-true (string-contains? h "graph.createBuffer(\"steady\", FrameMode::Shared, VkDeviceSize((in->m + 1u)) * sizeof(float))")))

(define gather
  (string-append
   "(compute-material Gather (workgroup 64)\n"
   "  (bindings (layers (array texture)) (sources (array (buffer-array uint))) (sums (buffer-array uint)))\n"
   "  (compute (let* ([i globalInvocationId.x] [layer (at layers i)])\n"
   "    (when (< i (array-length sources))\n"
   "      (write! (at sums i) (+ (at (at sources i) 0) (uint (.x (texture-size layer 0)))))))))\n"))

(test-case "Array bindings index their elements through a buffer of indices"
  (define outputs (compile-text gather))
  (define shader (named outputs "Gather.comp"))
  (check-true (string-contains? shader "#define layers(i) (bindlessTextures[nonuniformEXT(zgalIndices[materialPush.layers].data[1u + uint(i)])])"))
  (check-true (string-contains? shader "#define zgalLength_sources() (zgalIndices[materialPush.sources].data[0])"))
  ;; A texture bound with let* is an alias, as GLSL has no texture locals
  (check-true (string-contains? shader "textureSize(layers(i), 0)"))
  (define h (named outputs ".zgal.h"))
  (check-true (string-contains? h "Handle<DescriptorArray> layers;   // (array texture)"))
  (check-true (string-contains? h "MaterialBinding::array(std::move(bindings.sources), Access::StorageBufferRead, 0),"))
  ;; Writes through an element make the array's access a write
  (define written (string-replace gather "(when (< i (array-length sources))" "(when (< i (array-length sources)) (write! (at (at sources 0) i) 1)"))
  (check-true (string-contains? (named (compile-text written) ".zgal.h")
                                "MaterialBinding::array(std::move(bindings.sources), Access::StorageBufferReadWrite, 0),")))

(test-case "Graphs give array bindings lists of resources, or imported images"
  (define graph
    (string-append
     gather
     "(graph G (import-images pictures #:resting shader-read-only) (buffer a uint 1 #:frames shared) (buffer b uint 1 #:frames shared)\n"
     "  (material gather Gather (layers pictures) (sources (a b a)) (sums a) #:invocations (3))\n"
     "  (dispatch run gather))\n"))
  (define h (named (compile-text graph) ".zgal.h"))
  (check-true (string-contains? h "std::vector<FrameBinding<Image>> pictures;   // Imported, resting shader-read-only"))
  (check-true (string-contains? h "std::vector<ImageId> pictures;"))
  (check-true (string-contains? h "TRY_ASSIGN(auto array_gather_sources, DescriptorArray::create(context, {graph.storageBufferEntry(buffer_a), graph.storageBufferEntry(buffer_b), graph.storageBufferEntry(buffer_a)}));"))
  (check-true (string-contains? h "elements_gather_layers.push_back(graph.sampledEntry(image));"))
  (check-zgal-error (string-replace graph "(sources (a b a))" "(sources a)") 'binding-mismatch
                    "binding sources is an array, so it takes a list of resources, e.g. (sources (a b)), or imported images, not a")
  (check-zgal-error (string-replace graph "(sources (a b a))" "(sources pictures)") 'binding-mismatch
                    "binding sources is an array of buffer-arrays, so it takes a list of them, but pictures is imported images")
  (check-zgal-error (string-replace graph "(layers pictures)" "(layers (a))") 'binding-mismatch
                    "binding layers is an array of texture, so it takes an image, but a is a buffer"))

(test-case "Graphs choose array bindings' elements each frame with #:select"
  (define graph
    (string-append
     gather
     "(graph G (input picks (per-frame (data uint))) (import-images pictures #:resting shader-read-only)\n"
     "  (buffer a uint 1 #:frames shared) (buffer b uint 1 #:frames shared)\n"
     "  (material gather Gather (layers pictures #:select picks) (sources (a b) #:select picks) (sums a) #:invocations ((count picks)))\n"
     "  (dispatch run gather))\n"))
  (define h (named (compile-text graph) ".zgal.h"))
  (check-true (string-contains? h "std::vector<FrameBinding<DescriptorEntry>> elements_gather_sources{graph.storageBufferEntry(buffer_a), graph.storageBufferEntry(buffer_b)};"))
  (check-true (string-contains? h "TRY_ASSIGN(auto array_gather_sources, DescriptorArray::create(context, {}));"))
  (check-true (string-contains? h "graph.setElementSource(array_gather_sources, zgal::selectElements(std::move(elements_gather_sources), data_picks, \"G's material gather's binding sources\"));"))
  (check-true (string-contains? h "graph.setElementSource(array_gather_layers, zgal::selectElements(std::move(elements_gather_layers), data_picks,"))
  (check-zgal-error (string-replace graph "(sums a)" "(sums a #:select picks)") 'binding-mismatch
                    "binding sums isn't an array, so it has no elements to #:select")
  (check-zgal-error (string-replace graph "(input picks (per-frame (data uint)))" "(input picks (per-frame (data int)))") 'binding-mismatch
                    "binding layers's #:select is the elements' indices, a per-frame data input of uints ((input picks (per-frame (data uint)))), not picks")
  (check-zgal-error (string-replace graph "(sources (a b) #:select picks)" "(sources (a b) #:pick picks)") 'malformed
                    "expected a binding (binding resource), (binding resources #:select indices) for an array, or a binding named like its resource"))

(test-case "Arrays hold textures, storage images and buffers, and only they have a length"
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (s (array sampler))) (compute))" 'bad-binding
                    "binding s: an array holds textures, storage images or buffers, not samplers")
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (s (array))) (compute))" 'bad-binding
                    "binding s: expected (array kind)")
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (b (buffer-array uint))) (compute (write! (at b 0) (array-length b))))"
                    'type-mismatch "array-length gives an array binding's number of elements"))

(define phases
  (string-append
   "(compute-material Fill (workgroup 64) (bindings (cells (buffer-array uint)) (copies (buffer-array uint)))\n"
   "  (compute (write! (at copies 0) (at cells globalInvocationId.x))))\n"
   "(graph Setup (buffer cells uint 4 #:frames shared) (buffer copies uint 4 #:frames shared)\n"
   "  (material filling Fill cells copies #:invocations (4))\n"
   "  (dispatch fill filling #:once #t)\n"
   "  (readback readCells cells #:once #t))\n"
   "(graph Show (input cells (per-frame (data uint))) (upload cellBuffer cells)\n"
   "  (buffer copies uint (count cells) #:frames per-frame)\n"
   "  (material filling Fill (cells cellBuffer) copies #:invocations ((count cells)))\n"
   "  (dispatch fill filling))\n"
   "(app Phased (state (cells (data uint) (empty-data uint)) (ready bool #f))\n"
   "  (on (read readCells copied) (with state #:cells copied #:ready (> (count copied) 0)))\n"
   "  (run Setup #:when (not state.ready))\n"
   "  (run Show (cells state.cells)))\n"))

(test-case "Nodes can run once, in the graph's first frame"
  (define h (named (compile-text phases) ".zgal.h"))
  (check-true (string-contains? h "graph.runOnce(node);"))
  (check-zgal-error (string-replace phases "(dispatch fill filling #:once #t)" "(dispatch fill filling #:once #t #:enabled #t)")
                    'bad-option "node fill runs once, so it has no #:enabled condition")
  (check-zgal-error (string-replace phases "(run Setup #:when" "(run Setup #:enabled ((fill #t)) #:when")
                    'type-mismatch "fill runs once (#:once), so the app doesn't enable it"))

(test-case "Apps run #:once nodes again with run-once!"
  (define rerun (string-replace phases "(run Setup #:when" "(on (key-pressed r) (run-once! readCells) state)\n  (run Setup #:when"))
  (define h (named (compile-text rerun) ".zgal.h"))
  (check-true (string-contains? h "void runOnce_readCells() {\n        graph0_->runOnce(ids0_->nodes.readCells);\n    }"))
  (check-true (string-contains? h "runOnce_readCells();"))
  (check-zgal-error (string-replace rerun "(run-once! readCells)" "(run-once! fill)") 'type-mismatch "node fill isn't #:once #t")
  (check-zgal-error (string-replace rerun "(run-once! readCells)" "(run-once! nothing)") 'unknown-name "no graph app Phased runs has a node nothing")
  (check-zgal-error "(compute-material C (workgroup 1) (compute (run-once! x)))" 'bad-effect "run-once! is an effect of apps"))

(test-case "Apps run one of several graphs each frame, chosen from their state"
  (define h (named (compile-text phases) ".zgal.h"))
  (check-true (string-contains? h "RenderGraph* setup = nullptr;"))
  (check-true (string-contains? h "static Result<std::unique_ptr<Phased>> create(VulkanContext& context, Graphs graphs, Inputs inputs)"))
  (check-true (string-contains? h "if (active_Setup()) {"))
  (check-true (string-contains? h "const Show& showIds() const"))
  (check-zgal-error (string-replace phases "(run Show (cells state.cells))" "(run Show (cells state.cells) #:when #t)")
                    'bad-option "the last graph an app runs has no #:when")
  (check-zgal-error (string-replace phases "(run Setup #:when (not state.ready))" "(run Setup)")
                    'bad-option "gives every graph but the last a #:when condition")
  (check-zgal-error (string-replace phases "(run Show (cells state.cells)))" "(run Show (cells state.cells) #:when #t) (run Show (cells state.cells)))")
                    'duplicate-name "app Phased runs graph Show twice"))

(test-case "Readbacks' copies are events, whose data the state holds and graphs upload"
  (define h (named (compile-text phases) ".zgal.h"))
  (check-true (string-contains? h "zgal::Data<uint32_t> cells{};"))
  (check-true (string-contains? h "TRY_ASSIGN(auto copied, graph0_->nodeAs<BufferReadbackNode>(ids0_->nodes.readCells).takeFinished<uint32_t>()"))
  (check-true (string-contains? h "graph.add<zgal::DataUploadNode<uint32_t>>(\"upload cellBuffer\", buffer_cellBuffer, data_cells);"))
  (check-true (string-contains? h "data_cells->get(frame).size()"))
  (check-zgal-error (string-replace phases "(on (read readCells copied)" "(on (read fill copied)")
                    'type-mismatch "fill is a dispatch node, not a readback")
  (check-zgal-error (string-replace phases "(on (read readCells copied)" "(on (read nothing copied)")
                    'unknown-name "no graph app Phased runs has a node nothing")
  ;; Per-frame data's count is only known each frame
  (check-zgal-error (string-replace phases "(upload cellBuffer cells)" "(upload cellBuffer cells) (constant n (count cells))")
                    'not-build-time "cells is a per-frame data input, whose count is only known each frame")
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (b (buffer-array uint))) (compute (write! (at b 0) (count b))))"
                    'type-mismatch "count gives how many elements data has"))
