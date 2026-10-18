#lang racket/base
;; The C++ backend: a header for the wrapper (include/), declaring the program's structs with the
;; shaders' std430 layout, its vertex formats, and a class per material that builds the material
;; from exactly the bindings, push constants and shaders the .zgal file declares.

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "layout.rkt"
         "cpp-common.rkt"
         "cpp-app.rkt"
         "cpp-graph.rkt"
         "features.rkt"
         "print-body.rkt"
         "parse.rkt"
         "writer.rkt")

(provide emit-header)

(define (vertex-format-enum a)
  (define t (field-type a))
  (define storage (attribute-storage a))
  (define components (if (vector-t? t) (vector-t-size t) 1))
  (define kind (if (vector-t? t) (vector-t-kind t) (scalar-t-kind t)))
  (define bits (if storage (storage-bits storage) 32))
  (format "VK_FORMAT_~a_~a"
          (string-join (for/list ([c (in-list '("R" "G" "B" "A"))] [_ (in-range components)]) (format "~a~a" c bits)) "")
          (case (or storage kind)
            [(float float16) "SFLOAT"] [(int int8 int16) "SINT"] [(uint uint8 uint16) "UINT"]
            [(unorm8 unorm16) "UNORM"] [(snorm8 snorm16) "SNORM"])))

;; The C++ type of an attribute in vertex buffers: its type, or the integers it's stored as
(define (attribute-cpp-type a)
  (define t (field-type a))
  (define storage (attribute-storage a))
  (cond
    [(not storage) (cpp-type t)]
    [else
     (define signed? (memq storage '(snorm8 snorm16 int8 int16)))
     (define bits (storage-bits storage))
     (if (vector-t? t)
         (format "glm::~a~avec~a" (if signed? "i" "u") bits (vector-t-size t))
         (format "~aint~a_t" (if signed? "" "u") bits))]))

(define (stage-flags stages)
  (if (null? stages)
      "0"
      (string-join (for/list ([s (in-list stages)])
                     (case s
                       [(vertex) "VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT"]
                       [(fragment) "VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT"]))
                   " | ")))

(define (access-enum b)
  (match* ((binding-resource-kind b) (binding-access b))
    [((or 'texture 'sampler) _) "Access::SampledRead"]
    [('storage-image 'read) "Access::StorageRead"]
    [('storage-image 'write) "Access::StorageWrite"]
    [('storage-image 'read-write) "Access::StorageReadWrite"]
    [(_ 'read) "Access::StorageBufferRead"]
    [(_ 'write) "Access::StorageBufferWrite"]
    [(_ 'read-write) "Access::StorageBufferReadWrite"]))


;; The type of b's entries (an array's elements')
(define (descriptor-type b)
  (case (binding-resource-kind b)
    [(texture) "DescriptorType::SampledImage"]
    [(sampler) "DescriptorType::Sampler"]
    [(storage-image) "DescriptorType::StorageImage"]
    [else "DescriptorType::StorageBuffer"]))

;; The binding as declared, for comments
(define (describe-binding b)
  (define (describe-kind kind element access)
    (match kind
      ['texture "texture"]
      ['sampler "sampler"]
      ['storage-image (format "(storage-image ~a)" access)]
      ['array (format "(array ~a)" (describe-kind (binding-kind element) (binding-element element) access))]
      [k (format "(~a ~a ~a)" k (type-name element) access)]))
  (define kind (describe-kind (binding-kind b) (binding-element b) (binding-access b)))
  (if (null? (binding-stages b))
      kind
      (format "~a, ~a shader" kind (string-join (map symbol->string (binding-stages b)) " and "))))

;; Emits a struct whose members are at the given std430 offsets (relative to `base`), padding
;; between them explicitly, followed by checks that the C++ layout is that layout
(define (emit-placed-struct! w name qualified placed-fields base size at indent)
  (define (line text #:at [where at]) (emit! w (string-append indent text) #:at where))
  (line (format "struct ~a {" name))
  (define padding-count 0)
  ;; uint32_t padding where it's aligned (as it is unless a struct has 8- or 16-bit members),
  ;; otherwise bytes
  (define (pad! cursor bytes where)
    (when (> bytes 0)
      (line (if (and (zero? (remainder cursor 4)) (zero? (remainder bytes 4)))
                (format "    uint32_t zgalPadding~a_[~a]{};" padding-count (quotient bytes 4))
                (format "    uint8_t zgalPadding~a_[~a]{};" padding-count bytes))
            #:at where)
      (set! padding-count (add1 padding-count))))
  (define end
    (for/fold ([cursor base]) ([p (in-list placed-fields)])
      (define f (placed-field p))
      (pad! cursor (- (placed-offset p) cursor) (field-loc f))
      (line (format "    ~a;" (cpp-declaration (field-type f) (field-name f))) #:at (field-loc f))
      (+ (placed-offset p) (std430-size (field-type f)))))
  (pad! end (- (+ base size) end) at)
  (line "};")
  (line (format "static_assert(sizeof(~a) == ~a, ~a);" qualified size
                (cpp-string (format "~a must be ~a bytes, as in the shaders (std430). Is GLM_FORCE_DEFAULT_ALIGNED_GENTYPES defined?"
                                    qualified size))))
  (for ([p (in-list placed-fields)])
    (define f (placed-field p))
    (define offset (- (placed-offset p) base))
    (line (format "static_assert(offsetof(~a, ~a) == ~a, ~a);" qualified (field-name f) offset
                  (cpp-string (format "~a::~a must be at byte ~a, as in the shaders (std430)" qualified (field-name f) offset)))
          #:at (field-loc f))))

(define (emit-struct! w d)
  (define-values (placed-fields _end) (place-fields (struct-decl-fields d) 0))
  (emit! w "")
  (emit! w (format "// struct ~a: std430, as the shaders see it" (struct-decl-name d)))
  (emit-placed-struct! w (struct-decl-name d) (struct-decl-name d) placed-fields 0
                       (std430-size (struct-t d)) (struct-decl-loc d) ""))

;; Converts value, of the attribute's type, to how vertex buffers store it
(define (stored a value)
  (define t (field-type a))
  (define vector? (vector-t? t))
  (match (attribute-storage a)
    [#f value]
    ['float16 (format "~a(~a)" (if vector? "glm::packHalf" "glm::packHalf1x16") value)]
    [(and s (or 'unorm8 'unorm16))
     (if vector?
         (format "glm::packUnorm<~a>(~a)" (if (eq? s 'unorm8) "uint8_t" "uint16_t") value)
         (format "glm::packUnorm1x~a(~a)" (storage-bits s) value))]
    [(and s (or 'snorm8 'snorm16))
     (if vector?
         (format "glm::packSnorm<~a>(~a)" (if (eq? s 'snorm8) "int8_t" "int16_t") value)
         (format "static_cast<~a>(glm::packSnorm1x~a(~a))" (attribute-cpp-type a) (storage-bits s) value))]
    [_ (format "~a(~a)" (if vector? (attribute-cpp-type a) (format "static_cast<~a>" (attribute-cpp-type a))) value)]))

;; An attribute's #:default in C++, of its type
(define (default-cpp a)
  (define t (field-type a))
  (define kind (if (vector-t? t) (vector-t-kind t) (scalar-t-kind t)))
  (define (number n)
    (case kind
      [(float) (let ([x (exact->inexact n)]) (format "~af" (if (integer? x) (format "~a.0" (inexact->exact x)) x)))]
      [(uint) (format "~au" n)]
      [else (format "~a" n)]))
  (match (attribute-default a)
    [(list ns ...) (format "~a(~a)" (cpp-type t) (string-join (map number ns) ", "))]
    [n (number n)]))

;; fromModel: a model primitive's vertices, each attribute from what models give by its name, or its
;; default
(define (emit-from-model! w f)
  (define name (vertex-format-name f))
  (define attributes (vertex-format-attributes f))
  (define (gen text) (emit! w text))
  (define (source a)
    (define m (model-attribute-of a))
    (and m
         (let* ([member (format "primitive.~a[i]" (model-attribute-member m))]
                [size (model-attribute-size m)]
                [wanted (vector-t-size (field-type a))])
           (cond
             [(= wanted size) member]
             [(< wanted size) (format "glm::vec~a(~a)" wanted member)]
             [else (format "glm::vec~a(~a, ~af)" wanted member (model-attribute-fill m))]))))
  (gen "")
  (gen (format "    // A model primitive's vertices (graphs' models): ~a"
               (string-join (for/list ([a (in-list attributes)])
                              (define m (model-attribute-of a))
                              (format "~a from ~a" (field-name a)
                                      (cond [(and m (attribute-default a)) (format "~a (or its default)" (model-attribute-member m))]
                                            [m (model-attribute-member m)]
                                            [else "its default"])))
                            ", ")))
  (gen (format "    static Result<std::vector<~a>> fromModel(const ModelPrimitive& primitive) {" name))
  (for ([a (in-list attributes)] #:when (and (model-attribute-of a) (not (attribute-default a))))
    (define member (model-attribute-member (model-attribute-of a)))
    (unless (equal? member "positions")
      (emit! w (format "        if (primitive.~a.empty()) {" member) #:at (field-loc a))
      (emit! w (format "            return Error(ErrorCode::InvalidData, ~a);"
                       (cpp-string (format "the model has no ~a, which ~a's ~a reads (or give it a #:default)" member name (field-name a))))
             #:at (field-loc a))
      (emit! w "        }" #:at (field-loc a))))
  (gen (format "        std::vector<~a> vertices(primitive.positions.size());" name))
  (gen "        for (size_t i = 0; i < vertices.size(); ++i) {")
  (for ([a (in-list attributes)])
    (define from (source a))
    (define value
      (cond
        [(and from (attribute-default a))
         (format "primitive.~a.empty() ? ~a : ~a" (model-attribute-member (model-attribute-of a)) (default-cpp a) from)]
        [from from]
        [else (default-cpp a)]))
    (emit! w (format "            vertices[i].~a = ~a;" (field-name a)
                     (stored a (if (and from (attribute-default a)) (format "(~a)" value) value)))
           #:at (field-loc a)))
  (gen "        }")
  (gen "        return vertices;")
  (gen "    }"))

(define (emit-vertex-format! w f from-model?)
  (emit! w "")
  (emit! w (format "// vertex-format ~a: read from vertex buffers as C++ lays it out" (vertex-format-name f)))
  (emit! w (format "struct ~a {" (vertex-format-name f)) #:at (vertex-format-loc f))
  (for ([a (in-list (vertex-format-attributes f))])
    (define storage (attribute-storage a))
    (emit! w (if storage
                 ;; Packed attributes start at 4-byte boundaries, as Metal requires (so strides are
                 ;; multiples of 4 too)
                 (format "    alignas(4) ~a ~a;   // ~a, read as ~a~a" (attribute-cpp-type a) (field-name a)
                         storage (type-name (field-type a)) (packing-hint a))
                 (format "    ~a;" (cpp-declaration (field-type a) (field-name a))))
           #:at (field-loc a)))
  (when from-model?
    (emit-from-model! w f))
  (emit! w "};" #:at (vertex-format-loc f)))

;; How C++ converts floats to an attribute's storage (GLM's gtc/packing.hpp), for its comment
(define (packing-hint a)
  (define vector? (vector-t? (field-type a)))
  (define storage (attribute-storage a))
  (define (pack what type bits)
    (if vector? (format "glm::pack~a<~a>" what type) (format "glm::pack~a1x~a" what bits)))
  (case storage
    [(float16) (format "; convert with ~a" (if vector? "glm::packHalf" "glm::packHalf1x16"))]
    [(unorm8) (format "; convert with ~a" (pack "Unorm" "uint8_t" 8))]
    [(snorm8) (format "; convert with ~a" (pack "Snorm" "int8_t" 8))]
    [(unorm16) (format "; convert with ~a" (pack "Unorm" "uint16_t" 16))]
    [(snorm16) (format "; convert with ~a" (pack "Snorm" "int16_t" 16))]
    [else ""]))

(define (stage-bit stage)
  (case stage
    [(vertex) "VK_SHADER_STAGE_VERTEX_BIT"] [(fragment) "VK_SHADER_STAGE_FRAGMENT_BIT"]
    [(compute) "VK_SHADER_STAGE_COMPUTE_BIT"]))

(define (subgroup-bit category)
  (format "VK_SUBGROUP_FEATURE_~a_BIT" (string-upcase (symbol->string category))))

(define (emit-material! w prog m)
  (define name (material-name m))
  (define flags (material-feature-flags m prog))
  (define-values (subgroup-stages subgroup-operations) (material-subgroup-needs m prog))
  (define graphics? (eq? (material-kind m) 'graphics))
  (define bindings (material-bindings m))
  (define push (material-push m))
  (define shaders (material-shaders m))
  (define shader-members
    (for/list ([s (in-list shaders)]) (symbol->string (shader-stage s))))
  (define (gen text) (emit! w text))

  (gen "")
  (gen (format "// ~a ~a (~a)" (if graphics? "graphics-material" "compute-material") name
               (string-join (map shader-name shaders) ", ")))
  (emit! w (format "class ~a : public ~a {" name (if graphics? "GraphicsMaterial" "ComputeMaterial"))
         #:at (material-loc m))
  (gen "public:")

  ;; Bindings
  (unless (null? bindings)
    (gen "    // The descriptor entries the shaders use (an array's: DescriptorArray::create). Their")
    (gen "    // indices are pushed in this order.")
    (gen "    struct Bindings {")
    (for ([b (in-list bindings)])
      (emit! w (format "        ~a ~a;   // ~a" (if (eq? (binding-kind b) 'array) "Handle<DescriptorArray>" "FrameBinding<DescriptorEntry>")
                       (binding-name b) (describe-binding b))
             #:at (binding-loc b)))
    (gen "    };")
    (gen ""))

  ;; Push constants
  (unless (null? push)
    (define start (* 4 (length bindings)))
    (define-values (placed-fields end) (place-fields push start))
    (define base (placed-offset (first placed-fields)))
    (gen "    // Push constants after the binding indices, filled by writePush whenever the material is bound")
    (emit-placed-struct! w "Push" "Push" placed-fields base (- end base) (material-loc m) "    ")
    (gen (format "    // Where Push starts in the push constants, and where they end"))
    (gen (format "    static constexpr uint32_t kPushOffset = ~a;" base))
    (gen (format "    static constexpr uint32_t kPushEnd = ~a;" end))
    (gen "    static_assert(kPushEnd <= DescriptorHeap::kPushConstantSize);")
    (gen ""))

  ;; Shaders
  (gen (format "    // The shaders ~a is generated for (loadShaders), so it can't be built from shaders with"
               name))
  (gen "    // another interface")
  (gen (format "    using Shaders = zgal::~aShaders<~a>;" (if graphics? "Graphics" "Compute") name))
  (gen "")
  (gen (format "    // Loads ~a from the context's shader library" (string-join (map shader-name shaders) " and ")))
  (gen "    static Result<Shaders> loadShaders(VulkanContext& context) {")
  (when (pair? flags)
    (gen (format "        zgal::requireFeatures(context, requiredFeatures(), ~a);" (cpp-string name))))
  (when (pair? subgroup-stages)
    (gen (format "        zgal::requireSubgroups(context, kSubgroupStages, kSubgroupOperations, ~a);"
                 (cpp-string (format "~a's shaders use subgroup operations (~a) in stages (~a) the device's subgroups don't support"
                                     name (string-join (map symbol->string subgroup-operations) ", ")
                                     (string-join (map symbol->string subgroup-stages) ", "))))))
  (for ([s (in-list shaders)] [member (in-list shader-members)])
    (emit! w (format "        TRY_ASSIGN(auto ~a, context.shaders().get(~a));" member (cpp-string (shader-name s)))
           #:at (shader-loc s)))
  (gen (format "        return Shaders(~a);"
               (string-join (for/list ([member (in-list shader-members)]) (format "std::move(~a)" member)) ", ")))
  (gen "    }")
  (gen "")
  (gen "    // The device features the shaders need beyond the wrapper's: add them to the context's")
  (gen "    // (VulkanContextConfig::features, AppConfig::features) before creating it")
  (cond
    [(null? flags)
     (gen "    static DeviceFeatures requiredFeatures() { return {}; }")]
    [else
     (gen "    static DeviceFeatures requiredFeatures() {")
     (gen "        DeviceFeatures features;")
     (for ([flag (in-list flags)])
       (gen (format "        features.~a = VK_TRUE;" flag)))
     (gen "        return features;")
     (gen "    }")])
  (gen "")
  (unless (null? subgroup-stages)
    (gen "    // The stages using subgroup operations, and the operations: devices support them in some stages")
    (gen (format "    static constexpr VkShaderStageFlags kSubgroupStages = ~a;" (string-join (map stage-bit subgroup-stages) " | ")))
    (gen (format "    static constexpr VkSubgroupFeatureFlags kSubgroupOperations = ~a;"
                 (string-join (map subgroup-bit subgroup-operations) " | ")))
    (gen ""))

  (cond
    [graphics?
     (define input (material-vertex-input m))
     (gen "    // The colour attachments the fragment shader writes: its targets' colour formats")
     (gen (format "    static constexpr uint32_t kColorOutputs = ~a;" (length (material-outputs m))))
     (gen "")
     (when input
       (define f (vertex-input-format input))
       (gen (format "    // Vertex buffers of ~a (one, at binding 0), read at the attributes' locations"
                    (vertex-format-name f)))
       (gen "    static VertexLayout vertexLayout() {")
       (gen (format "        return VertexLayout::interleaved(sizeof(~a), {" (vertex-format-name f)))
       (for ([entry (in-list (vertex-input-attributes input))])
         (match-define (cons index a) entry)
         (emit! w (format "            {~a, 0, ~a, offsetof(~a, ~a)}," index (vertex-format-enum a)
                          (vertex-format-name f) (field-name a))
                #:at (field-loc a)))
       (gen "        });")
       (gen "    }")
       (gen ""))
     (gen (format "    // formats must have kColorOutputs colour formats. options.vertexLayout must be empty: ~a"
                  (if input "the material's vertex-input sets it." "the material has no vertex-input.")))
     (gen (format "    ~a(VulkanContext& context, const Shaders& shaders, ~aAttachmentFormats formats," name
                  (if (null? bindings) "" "Bindings bindings, ")))
     (gen "        GraphicsPipelineOptions options)")
     (gen (format "    : GraphicsMaterial(context, ~a, zgal::checkFormats(std::move(formats), kColorOutputs, ~a),"
                  (if (null? bindings) "{}" "materialBindings(std::move(bindings))") (cpp-string name)))
     (gen (format "                       shaders.vertex_, shaders.fragment_, zgal::withVertexInput(std::move(options), ~a, ~a, ~a)) {}"
                  (if input "vertexLayout()" "std::nullopt") (cpp-string name) (cpp-string (program-source prog))))]
    [else
     (match-define (list x y z) (material-workgroup m))
     (gen "    // The compute shader's workgroup size")
     (gen (format "    static constexpr uint32_t kWorkgroupSizeX = ~a;" x))
     (gen (format "    static constexpr uint32_t kWorkgroupSizeY = ~a;" y))
     (gen (format "    static constexpr uint32_t kWorkgroupSizeZ = ~a;" z))
     (gen "")
     (gen "    // The workgroups covering x * y * z invocations, for getDispatchDimensions. Rounded up, so")
     (gen "    // the shader must skip invocations past the end.")
     (gen "    static glm::vec3 workgroupsFor(uint32_t x, uint32_t y = 1, uint32_t z = 1) {")
     (gen "        return {float((x + kWorkgroupSizeX - 1) / kWorkgroupSizeX), float((y + kWorkgroupSizeY - 1) / kWorkgroupSizeY),")
     (gen "                float((z + kWorkgroupSizeZ - 1) / kWorkgroupSizeZ)};")
     (gen "    }")
     (gen "")
     (gen (format "    ~a(VulkanContext& context, const Shaders& shaders~a)" name
                  (if (null? bindings) "" ", Bindings bindings")))
     (gen (format "    : ComputeMaterial(context, ~a, shaders.compute_) {}"
                  (if (null? bindings) "{}" "materialBindings(std::move(bindings))")))])

  ;; Push constants and descriptor type checks
  (gen "")
  (gen "protected:")
  (unless (null? push)
    (gen "    // Fills the push constants for a frame; called whenever the material is bound")
    (gen "    virtual void writePush(uint32_t frameIndex, Push& push) = 0;")
    (gen ""))
  (gen "    void writePushConstants(uint32_t frameIndex, std::vector<uint8_t>& data) final {")
  (unless (null? bindings)
    (gen (format "        static constexpr DescriptorType kTypes[] = {~a};"
                 (string-join (map descriptor-type bindings) ", ")))
    (gen (format "        static constexpr const char* kNames[] = {~a};"
                 (string-join (map (λ (b) (cpp-string (binding-name b))) bindings) ", ")))
    (gen (format "        zgal::checkDescriptorTypes(bindings(), frameIndex, kTypes, kNames, ~a);" (cpp-string name))))
  (unless (null? push)
    (gen "        Push push{};")
    (gen "        writePush(frameIndex, push);")
    (gen "        data.resize(kPushOffset + sizeof(Push));")
    (gen "        std::memcpy(data.data() + kPushOffset, &push, sizeof(Push));"))
  (gen "    }")
  (gen "")
  (unless (null? bindings)
    (gen "private:")
    (gen "    static std::vector<MaterialBinding> materialBindings(Bindings bindings) {")
    (for ([b (in-list bindings)])
      (gen (format "        zgal::requireBound(bindings.~a, ~a, ~a);" (binding-name b) (cpp-string name)
                   (cpp-string (binding-name b)))))
    (gen "        return {")
    (for ([b (in-list bindings)])
      (emit! w (if (eq? (binding-kind b) 'array)
                   (format "            MaterialBinding::array(std::move(bindings.~a), ~a, ~a)," (binding-name b) (access-enum b)
                           (stage-flags (binding-stages b)))
                   (format "            MaterialBinding{std::move(bindings.~a), ~a, ~a}," (binding-name b) (access-enum b)
                           (stage-flags (binding-stages b))))
             #:at (binding-loc b)))
    (gen "        };")
    (gen "    }"))
  (emit! w "};" #:at (material-loc m)))

;; The header for program prog, written to out-path
(define (emit-header prog out-path)
  (define w (make-writer out-path))
  (define materials (program-materials prog))
  (define (gen text) (emit! w text))
  (gen (format "// Generated by zgal from ~a. Don't edit it: edit that." (program-source prog)))
  (gen "// #line directives map declarations to their lines there, so errors in them point there.")
  (gen "#pragma once")
  (gen "")
  (define graphs (program-graphs prog))
  (when (pair? materials)
    (gen "#include \"zgal/Material.h\""))
  (define apps (program-apps prog))
  (when (pair? graphs)
    (for ([h (in-list '("graph/RenderGraph.h" "graph/nodes/DispatchNode.h" "graph/nodes/DispatchSequenceNode.h" "graph/nodes/IndirectDrawNode.h" "graph/nodes/ReadbackNode.h"
                        "graph/nodes/RenderablesNode.h" "graph/nodes/BufferReadbackNode.h" "materials/Renderable.h" "zgal/Runtime.h"))])
      (gen (format "#include \"~a\"" h)))
    (when (for*/or ([g (in-list graphs)] [item (in-list (graph-items g))]) (g-model? item))
      (gen "#include \"zgal/Assets.h\"")))
  (when (pair? apps)
    (when (null? graphs) (gen "#include \"graph/RenderGraph.h\""))
    (gen "#include \"zgal/App.h\""))
  (gen "")
  (gen "#include <glm/glm.hpp>")
  (when (ormap vertex-format-packed? (program-vertex-formats prog))
    (gen "#include <glm/gtc/packing.hpp>")
    (gen "#include <glm/gtc/type_precision.hpp>"))
  (gen "")
  (gen "#include <array>")
  (gen "#include <cstddef>")
  (gen "#include <cstdint>")
  (when (pair? materials)
    (gen "#include <cstring>")
    (gen "#include <string>")
    (gen "#include <utility>")
    (gen "#include <vector>"))
  (when (or (pair? graphs) (pair? apps))
    (gen "#include <algorithm>")
    (gen "#include <memory>")
    (gen "#include <optional>"))
  (when (pair? apps)
    (gen "#include <functional>")
    (gen "#include <iostream>")
    (gen "#include <variant>"))
  (gen "")
  (gen (format "// Everything generated from ~a is in its namespace" (program-source prog)))
  (gen (format "namespace ~a {" (program-namespace prog)))
  (for ([d (in-list (program-structs prog))]) (emit-struct! w d))
  ;; Formats graphs' models load convert from them
  (define model-formats
    (for*/list ([g (in-list (program-graphs prog))] [item (in-list (graph-items g))] #:when (g-model? item))
      (vertex-format-name (g-model-format item))))
  (for ([f (in-list (program-vertex-formats prog))])
    (emit-vertex-format! w f (and (member (vertex-format-name f) model-formats) #t)))
  ;; Functions and constants the CPU can use too
  (define cpu-declarations
    (filter (λ (d) (if (function-decl? d) (function-decl-cpu? d) (constant-decl-cpu? d))) (program-functions prog)))
  (unless (null? cpu-declarations)
    (gen "")
    (gen "// The file's functions and constants, compiled for the CPU too (those using no textures,")
    (gen "// images or stage variables)")
    (for ([d (in-list cpu-declarations)])
      (if (function-decl? d) (emit-function! w d 'cpp) (emit-constant! w d 'cpp))))
  (for ([m (in-list materials)]) (emit-material! w prog m))
  (for ([g (in-list graphs)]) (emit-graph! w prog g))
  (for ([a (in-list apps)]) (emit-app! w prog a))
  (gen "")
  (gen (format "} // namespace ~a" (program-namespace prog)))
  (writer-text w))
