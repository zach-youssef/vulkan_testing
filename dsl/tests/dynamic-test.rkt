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

(test-case "Arrays hold textures, storage images and buffers, and only they have a length"
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (s (array sampler))) (compute))" 'bad-binding
                    "binding s: an array holds textures, storage images or buffers, not samplers")
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (s (array))) (compute))" 'bad-binding
                    "binding s: expected (array kind)")
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (b (buffer-array uint))) (compute (write! (at b 0) (array-length b))))"
                    'type-mismatch "array-length gives an array binding's number of elements"))
