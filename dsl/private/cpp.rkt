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
         "cpp-graph.rkt"
         "print-body.rkt"
         "parse.rkt"
         "writer.rkt")

(provide emit-header)

(define (vertex-format-enum t)
  (define components (if (vector-t? t) (vector-t-size t) 1))
  (define kind (if (vector-t? t) (vector-t-kind t) (scalar-t-kind t)))
  (format "VK_FORMAT_~a_~a"
          (string-join (for/list ([c (in-list '("R32" "G32" "B32" "A32"))] [_ (in-range components)]) c) "")
          (case kind [(float) "SFLOAT"] [(int) "SINT"] [(uint) "UINT"])))

(define (stage-flags stages)
  (if (null? stages)
      "0"
      (string-join (for/list ([s (in-list stages)])
                     (case s
                       [(vertex) "VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT"]
                       [(fragment) "VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT"]))
                   " | ")))

(define (access-enum b)
  (match* ((binding-kind b) (binding-access b))
    [((or 'texture 'sampler) _) "Access::SampledRead"]
    [('storage-image 'read) "Access::StorageRead"]
    [('storage-image 'write) "Access::StorageWrite"]
    [('storage-image 'read-write) "Access::StorageReadWrite"]
    [(_ 'read) "Access::StorageBufferRead"]
    [(_ 'write) "Access::StorageBufferWrite"]
    [(_ 'read-write) "Access::StorageBufferReadWrite"]))


(define (descriptor-type b)
  (case (binding-kind b)
    [(texture) "DescriptorType::SampledImage"]
    [(sampler) "DescriptorType::Sampler"]
    [(storage-image) "DescriptorType::StorageImage"]
    [else "DescriptorType::StorageBuffer"]))

;; The binding as declared, for comments
(define (describe-binding b)
  (define kind
    (match (binding-kind b)
      ['texture "texture"]
      ['sampler "sampler"]
      ['storage-image (format "(storage-image ~a)" (binding-access b))]
      [k (format "(~a ~a ~a)" k (type-name (binding-element b)) (binding-access b))]))
  (if (null? (binding-stages b))
      kind
      (format "~a, ~a shader" kind (string-join (map symbol->string (binding-stages b)) " and "))))

;; Emits a struct whose members are at the given std430 offsets (relative to `base`), padding
;; between them explicitly, followed by checks that the C++ layout is that layout
(define (emit-placed-struct! w name qualified placed-fields base size at indent)
  (define (line text #:at [where at]) (emit! w (string-append indent text) #:at where))
  (line (format "struct ~a {" name))
  (define padding-count 0)
  (define (pad! bytes where)
    (when (> bytes 0)
      (line (format "    uint32_t zgalPadding~a_[~a]{};" padding-count (quotient bytes 4)) #:at where)
      (set! padding-count (add1 padding-count))))
  (define end
    (for/fold ([cursor base]) ([p (in-list placed-fields)])
      (define f (placed-field p))
      (pad! (- (placed-offset p) cursor) (field-loc f))
      (line (format "    ~a;" (cpp-declaration (field-type f) (field-name f))) #:at (field-loc f))
      (+ (placed-offset p) (std430-size (field-type f)))))
  (pad! (- (+ base size) end) at)
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

(define (emit-vertex-format! w f)
  (emit! w "")
  (emit! w (format "// vertex-format ~a: read from vertex buffers as C++ lays it out" (vertex-format-name f)))
  (emit! w (format "struct ~a {" (vertex-format-name f)) #:at (vertex-format-loc f))
  (for ([a (in-list (vertex-format-attributes f))])
    (emit! w (format "    ~a;" (cpp-declaration (field-type a) (field-name a))) #:at (field-loc a)))
  (emit! w "};" #:at (vertex-format-loc f)))

(define (emit-material! w prog m)
  (define name (material-name m))
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
    (gen "    // The descriptor entries the shaders use. Their indices are pushed in this order.")
    (gen "    struct Bindings {")
    (for ([b (in-list bindings)])
      (emit! w (format "        FrameBinding<DescriptorEntry> ~a;   // ~a" (binding-name b) (describe-binding b))
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
  (gen "    class Shaders {")
  (gen "    public:")
  (for ([member (in-list shader-members)])
    (gen (format "        const Handle<Shader>& ~a() const { return ~a_; }" member member)))
  (gen "")
  (gen "    private:")
  (gen (format "        friend class ~a;" name))
  (gen (format "        explicit Shaders(~a)"
               (string-join (for/list ([member (in-list shader-members)]) (format "Handle<Shader> ~a" member)) ", ")))
  (gen (format "        : ~a {}"
               (string-join (for/list ([member (in-list shader-members)]) (format "~a_(std::move(~a))" member member)) ", ")))
  (for ([member (in-list shader-members)])
    (gen (format "        Handle<Shader> ~a_;" member)))
  (gen "    };")
  (gen "")
  (gen (format "    // Loads ~a from the context's shader library" (string-join (map shader-name shaders) " and ")))
  (gen "    static Result<Shaders> loadShaders(VulkanContext& context) {")
  (for ([s (in-list shaders)] [member (in-list shader-members)])
    (emit! w (format "        TRY_ASSIGN(auto ~a, context.shaders().get(~a));" member (cpp-string (shader-name s)))
           #:at (shader-loc s)))
  (gen (format "        return Shaders(~a);"
               (string-join (for/list ([member (in-list shader-members)]) (format "std::move(~a)" member)) ", ")))
  (gen "    }")
  (gen "")

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
         (emit! w (format "            {~a, 0, ~a, offsetof(~a, ~a)}," index (vertex-format-enum (field-type a))
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
     (gen (format "    : GraphicsMaterial(context, ~a, checkFormats(std::move(formats)), shaders.vertex_,"
                  (if (null? bindings) "{}" "materialBindings(std::move(bindings))")))
     (gen "                       shaders.fragment_, withVertexInput(std::move(options))) {}")]
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
    (gen "        checkDescriptorTypes(frameIndex);"))
  (unless (null? push)
    (gen "        Push push{};")
    (gen "        writePush(frameIndex, push);")
    (gen "        data.resize(kPushOffset + sizeof(Push));")
    (gen "        std::memcpy(data.data() + kPushOffset, &push, sizeof(Push));"))
  (gen "    }")
  (gen "")
  (gen "private:")
  (unless (null? bindings)
    (gen "    static std::vector<MaterialBinding> materialBindings(Bindings bindings) {")
    (for ([b (in-list bindings)])
      (gen (format "        requireBound(bindings.~a, ~a);" (binding-name b) (cpp-string (binding-name b)))))
    (gen "        return {")
    (for ([b (in-list bindings)])
      (emit! w (format "            MaterialBinding{std::move(bindings.~a), ~a, ~a}," (binding-name b) (access-enum b)
                       (stage-flags (binding-stages b)))
             #:at (binding-loc b)))
    (gen "        };")
    (gen "    }")
    (gen "")
    (gen "    static void requireBound(const FrameBinding<DescriptorEntry>& binding, const char* name) {")
    (gen "        if (!binding.isPerFrame() && !binding.shared()) {")
    (gen (format "            contractViolation(std::string(~a) + name + \"' has no descriptor entry\");"
                 (cpp-string (format "~a's binding '" name))))
    (gen "        }")
    (gen "    }")
    (gen "")
    (gen "    // A binding given an entry of the wrong kind would index the wrong descriptor array")
    (gen "    void checkDescriptorTypes(uint32_t frameIndex) {")
    (gen (format "        static constexpr DescriptorType kTypes[] = {~a};"
                 (string-join (map descriptor-type bindings) ", ")))
    (gen (format "        static constexpr const char* kNames[] = {~a};"
                 (string-join (map (λ (b) (cpp-string (binding-name b))) bindings) ", ")))
    (gen "        for (size_t i = 0; i < bindings().size(); ++i) {")
    (gen "            auto entry = bindings()[i].entry.get(frameIndex);")
    (gen "            if (entry && entry->type() != kTypes[i]) {")
    (gen (format "                contractViolation(std::string(~a) + kNames[i] + \"' needs a \" + descriptorTypeName(kTypes[i])"
                 (cpp-string (format "~a's binding '" name))))
    (gen "                                  + \" entry, but was given a \" + descriptorTypeName(entry->type()) + \" entry\");")
    (gen "            }")
    (gen "        }")
    (gen "    }")
    (gen ""))
  (when graphics?
    (gen "    static AttachmentFormats checkFormats(AttachmentFormats formats) {")
    (gen "        if (formats.color.size() != kColorOutputs) {")
    (gen (format "            contractViolation(~a + std::to_string(kColorOutputs) + \" colour attachments, but its target has \""
                 (cpp-string (format "~a's fragment shader writes " name))))
    (gen "                              + std::to_string(formats.color.size()));")
    (gen "        }")
    (gen "        return formats;")
    (gen "    }")
    (gen "")
    (gen "    static GraphicsPipelineOptions withVertexInput(GraphicsPipelineOptions options) {")
    (gen "        if (options.vertexLayout) {")
    (gen (format "            contractViolation(~a);"
                 (cpp-string (format "~a's vertex input is declared in ~a: leave GraphicsPipelineOptions::vertexLayout empty"
                                     name (program-source prog)))))
    (gen "        }")
    (when (material-vertex-input m)
      (gen "        options.vertexLayout = vertexLayout();"))
    (gen "        return options;")
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
  (cond
    [(ormap (λ (m) (eq? (material-kind m) 'graphics)) materials) (gen "#include \"materials/GraphicsMaterial.h\"")]
    [(pair? materials) (gen "#include \"materials/Material.h\"")])
  (when (pair? graphs)
    (for ([h (in-list '("graph/RenderGraph.h" "graph/nodes/DispatchNode.h" "graph/nodes/ReadbackNode.h"
                        "graph/nodes/RenderablesNode.h" "materials/Renderable.h" "zgal/Runtime.h"))])
      (gen (format "#include \"~a\"" h))))
  (gen "")
  (gen "#include <glm/glm.hpp>")
  (gen "")
  (gen "#include <array>")
  (gen "#include <cstddef>")
  (gen "#include <cstdint>")
  (when (pair? materials)
    (gen "#include <cstring>")
    (gen "#include <string>")
    (gen "#include <utility>")
    (gen "#include <vector>"))
  (when (pair? graphs)
    (gen "#include <algorithm>")
    (gen "#include <memory>")
    (gen "#include <optional>"))
  (for ([d (in-list (program-structs prog))]) (emit-struct! w d))
  (for ([f (in-list (program-vertex-formats prog))]) (emit-vertex-format! w f))
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
  (writer-text w))
