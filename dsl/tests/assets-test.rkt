#lang racket/base
;; Assets: packed vertex attributes, image files and models

(require racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (named outputs suffix)
  (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) suffix)) o)))

(define packed
  (string-append
   "(vertex-format V (position vec3) (normal vec3 #:as snorm16) (color vec4 #:as unorm8) (id uint #:as uint8) (shade float #:as float16))\n"
   "(graphics-material M (vertex-input V) (outputs (o vec4))\n"
   "  (vertex (results #:position (vec4 (+ position normal (.xyz color) (vec3 shade (float id) 0.0)) 1.0)))\n"
   "  (fragment (vec4 1.0)))\n"))

(test-case "Vertex attributes stored in fewer bits are read as their type"
  (define outputs (compile-text packed))
  (define h (named outputs ".zgal.h"))
  (check-true (string-contains? h "alignas(4) glm::i16vec3 normal;   // snorm16, read as vec3; convert with glm::packSnorm<int16_t>"))
  (check-true (string-contains? h "alignas(4) uint8_t id;   // uint8, read as uint"))
  (check-true (string-contains? h "alignas(4) uint16_t shade;   // float16, read as float; convert with glm::packHalf1x16"))
  (check-true (string-contains? h "{1, 0, VK_FORMAT_R16G16B16_SNORM, offsetof(V, normal)},"))
  (check-true (string-contains? h "{2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(V, color)},"))
  (check-true (string-contains? h "{4, 0, VK_FORMAT_R16_SFLOAT, offsetof(V, shade)},"))
  (check-true (string-contains? h "#include <glm/gtc/packing.hpp>"))
  ;; Shaders read the attributes' types
  (check-true (string-contains? (named outputs "M.vert") "layout(location = 1) in vec3 inNormal;")))

(test-case "An attribute's storage converts to its kind"
  (check-zgal-error "(vertex-format V (id uint #:as unorm8))" 'type-not-allowed
                    "id in V is read as uint, so it's stored as uint8, uint16, not unorm8")
  (check-zgal-error "(vertex-format V (p vec3 #:as half))" 'bad-option
                    "attribute p's #:as is one of float16, unorm8, snorm8, unorm16, snorm16, uint8, uint16, int8, int16, not half")
  (check-zgal-error "(vertex-format V (p vec3 #:stride 4))" 'bad-option "attribute p takes no #:stride option"))

(define files
  (string-append
   "(compute-material Copy (workgroup 1) (bindings (picture texture) (s sampler) (target (storage-image)))\n"
   "  (compute (store! target (ivec2 0) (sample picture s (vec2 0.5)))))\n"
   "(graph G (input chosen path) (parameter n uint)\n"
   "  (image-file logo \"logo.png\" #:color srgb)\n"
   "  (image-file data chosen #:color linear)\n"
   "  (sampler s)\n"
   "  (image canvas rgba8 (extent-of data) #:frames shared)\n"
   "  (material copy Copy (picture logo) s (target canvas) #:invocations ((width logo)))\n"
   "  (dispatch run copy))\n"))

(test-case "Image files load from paths, or path inputs, when the graph is built"
  (define h (named (compile-text files) ".zgal.h"))
  (check-true (string-contains? h "std::filesystem::path chosen;"))
  (check-true (string-contains? h "TRY_ASSIGN(auto file_logo, zgal::loadImage(context, std::filesystem::path(\"logo.png\"), ColorSpace::Srgb, \"G's image-file logo\"));"))
  (check-true (string-contains? h "TRY_ASSIGN(auto file_data, zgal::loadImage(context, in->chosen, ColorSpace::Linear, \"G's image-file data\"));"))
  (check-true (string-contains? h "const ImageId image_logo = graph.importImage(\"logo\", file_logo, ImageState::shaderReadOnly());"))
  ;; Their sizes are known when the graph is built
  (check-true (string-contains? h "FixedExtent{zgal::extentOf(context, file_data, \"data\")}"))
  (check-true (string-contains? h "zgal::extentOf(context, file_logo, \"logo\").width")))

(test-case "Image files have a colour space and a path, and are only sampled"
  (check-zgal-error (string-replace files " #:color srgb" "") 'bad-option "image-file needs #:color (srgb, linear)")
  (check-zgal-error (string-replace files "\"logo.png\"" "n") 'type-mismatch "n is a parameter, not a path input")
  (check-zgal-error (string-replace files "\"logo.png\"" "3") 'malformed "expected an asset's path, a string or a path input, found 3")
  (check-zgal-error (string-replace files "\"logo.png\"" "\"\"") 'malformed "an asset's path isn't empty")
  (check-zgal-error (string-replace files "(target canvas)" "(target logo)") 'binding-mismatch
                    "binding target is a storage image, but logo is an image file, which shaders only sample"))

(define buffer-files
  (string-append
   "(struct Pair (a uint) (b uint))\n"
   "(compute-material Bump (workgroup 1) (bindings (pairs (buffer-array Pair)))\n"
   "  (compute (let* ([p (at pairs 0)]) (write! (at pairs 0) (with p #:a (+ p.a 1))))))\n"
   "(graph G (input more path)\n"
   "  (buffer-file pairs Pair \"pairs.bin\" more)\n"
   "  (material bump Bump pairs #:invocations (1))\n"
   "  (dispatch run bump)\n"
   "  (readback read pairs))\n"))

(test-case "Buffer files read files' elements into a buffer when the graph is built"
  (define h (named (compile-text buffer-files) ".zgal.h"))
  (check-true (string-contains? h "TRY_ASSIGN(auto file_pairs, zgal::loadBufferFile<Pair>(context, {std::filesystem::path(\"pairs.bin\"), in->more}, \"G's buffer-file pairs\"));"))
  (check-true (string-contains? h "const BufferId buffer_pairs = graph.importBuffer<Pair>(\"pairs\", file_pairs);"))
  (check-true (string-contains? h "BufferId pairs;"))
  (check-zgal-error (string-replace buffer-files " \"pairs.bin\" more" "") 'malformed "expected (buffer-file name type asset ...+)")
  (check-zgal-error (string-replace buffer-files "Pair \"pairs.bin\"" "(array Pair 2) \"pairs.bin\"") 'type-not-allowed "give the element type")
  (check-zgal-error (string-append buffer-files "(graph S (buffer-file keys uint \"k.bin\") (buffer n uint 1 #:frames shared) (sort s keys #:count (at n 0)))")
                    'type-mismatch "sort's keys keys are read from files, whose size the graph doesn't know"))

(define models
  (string-append
   "(vertex-format V (position vec3) (normal vec3 #:as snorm8) (colour vec3 #:default (vec3 1 1 1)) (id uint #:default 3))\n"
   "(graphics-material Paint (vertex-input V) (bindings (colors (array texture)) (factors (buffer-array vec4)) (s sampler))\n"
   "  (varyings (m uint flat)) (outputs (o vec4))\n"
   "  (vertex (results #:position (vec4 (+ position normal colour) 1.0) #:m (+ id (uint instanceIndex))))\n"
   "  (fragment (* (at factors m) (sample (at colors m) s (vec2 0.5)))))\n"
   "(graph G (input file path)\n"
   "  (model ship file V #:base-colors shipColors #:base-color-factors shipFactors)\n"
   "  (sampler s) (image canvas rgba8 (fixed 4 4) #:frames shared) (target t (color canvas))\n"
   "  (material paint Paint (colors shipColors) (factors shipFactors) s)\n"
   "  (draw-meshes draw t ship paint))\n"))

(test-case "Models fill their vertex format by name, and give their materials' base colours"
  (define h (named (compile-text models) ".zgal.h"))
  (check-true (string-contains? h "#include \"zgal/Assets.h\""))
  (check-true (string-contains? h "static Result<std::vector<V>> fromModel(const ModelPrimitive& primitive) {"))
  (check-true (string-contains? h "vertices[i].normal = glm::packSnorm<int8_t>(primitive.normals[i]);"))
  ;; A default fills a named attribute the file lacks, or an attribute models don't give
  (check-true (string-contains? h "vertices[i].colour = (primitive.colors.empty() ? glm::vec3(1.0f, 1.0f, 1.0f) : glm::vec3(primitive.colors[i]));"))
  (check-true (string-contains? h "vertices[i].id = 3u;"))
  (check-true (string-contains? h "TRY_ASSIGN(auto model_ship, zgal::loadModel<V>(context, in->file, \"G's model ship\"));"))
  (check-true (string-contains? h "zgal::importImages(graph, \"shipColors\", model_ship.baseColorTextures, ImageState::shaderReadOnly())"))
  (check-true (string-contains? h "graph.importBuffer<glm::vec4>(\"shipFactors\", upload_shipFactors)"))
  (check-true (string-contains? h "renderables = model_ship.renderables(context, ")))

(test-case "Models need vertex formats they can fill, read by the materials drawing them"
  (check-zgal-error (string-replace models " (id uint #:default 3)" " (id uint)") 'model-attribute
                    "models don't give attribute id of V: name it after what they give (position, normal, uv, color, tangent) or give it a #:default")
  (check-zgal-error (string-replace (string-replace models "(normal vec3 #:as snorm8)" "(normal vec2)") "(+ position normal colour)" "(+ position colour)")
                    'model-attribute
                    "attribute normal of V takes models' normals, so it's vec3 or vec4, not vec2")
  (check-zgal-error (string-replace models "(id uint #:default 3)" "(id uint #:default -1)") 'type-mismatch
                    "attribute id's #:default is uint, so its components are uint numbers, not -1")
  (check-zgal-error (string-replace models "#:default (vec3 1 1 1)" "#:default (vec4 1 1 1 1)") 'type-mismatch
                    "attribute colour's #:default is a vec3, e.g. (vec3 0 0 0), not (vec4 1 1 1 1)")
  (check-zgal-error (string-replace models "(model ship file V" "(model ship file W") 'unknown-name
                    "W isn't a vertex format declared earlier in the file")
  (define other
    (string-replace (string-replace models "(graphics-material Paint (vertex-input V)" "(vertex-format W (position vec3))\n(graphics-material Paint (vertex-input W)")
                    "(+ position normal colour)" "position"))
  (check-zgal-error (string-replace other "(+ id (uint instanceIndex))" "(uint instanceIndex)") 'type-mismatch
                    "model ship's vertices are V, but material paint (Paint) reads W"))
