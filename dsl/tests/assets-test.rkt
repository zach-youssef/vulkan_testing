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
