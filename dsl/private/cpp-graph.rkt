#lang racket/base
;; The C++ backend for graphs: a struct per graph, whose build() adds the graph's resources,
;; materials and nodes to a RenderGraph, using the runtime in include/zgal/Runtime.h

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "builtins.rkt"
         "cpp-common.rkt"
         "graph.rkt"
         "writer.rkt")

(provide emit-graph!
         graph-input-members
         item-loc)

;; The graph being emitted, for messages
(define current-graph-name (make-parameter #f))

;; The captures of a lambda computing expressions each frame: by copy, plus the context or graph
;; when an image's size needs them (capturing them only then, as compilers warn about unused
;; captures)
(define (frame-captures exprs env)
  (define (images-sized kind)
    (let walk ([e exprs])
      (match e
        [(list es ...) (ormap walk es)]
        [(e-op (or 'width 'height) (list (e-ref name _)) _) (eq? (entry-kind (hash-ref env name)) kind)]
        [(e-op _ args _) (walk args)]
        [_ #f])))
  (string-append "=" (if (images-sized 'import) ", &context" "") (if (images-sized 'image) ", &graph" "")))

;; ---------------------------------------------------------------------------------------------
;; Expressions

;; The C++ type of a parameter, constant or expression (bools are C++ bools here, not std430's)
(define (value-cpp-type t)
  (match t
    [(scalar-t 'bool) "bool"]
    [_ (cpp-type t)]))

(define (resolve t want)
  (if (equal? t literal-type) (or want (scalar-t 'int)) t))

(define (literal->cpp v t)
  (cond
    [(boolean? v) (if v "true" "false")]
    [else
     (match t
       [(scalar-t 'float)
        (define text (number->string (exact->inexact v)))
        (string-append (if (regexp-match? #rx"[.e]" text) text (string-append text ".0")) "f")]
       [(scalar-t 'uint) (format "~au" v)]
       [_ (format "~a" v)])]))

;; e as C++, of type `want` (which integer literals take). Parameters are read from `in` (the
;; inputs), per-frame inputs called with `frame`.
(define (cpp-expr e env [want #f])
  (define (type-of e) (expression-type e env))
  (match e
    [(e-literal v _) (literal->cpp v (resolve (if (boolean? v) (scalar-t 'bool) (if (exact-integer? v) literal-type (scalar-t 'float))) want))]
    [(e-ref name _)
     (match (entry-kind (hash-ref env name))
       ['parameter (format "in->~a" name)]
       ['constant (format "constant_~a" name)]
       ['per-frame (format "in->~a(frame)" name)])]
    [(e-op op args _)
     (define t (resolve (type-of e) want))
     (case op
       [(if) (format "(~a ? ~a : ~a)" (cpp-expr (first args) env (scalar-t 'bool))
                     (cpp-expr (second args) env t) (cpp-expr (third args) env t))]
       [(and or) (format "(~a)" (string-join (map (λ (a) (cpp-expr a env (scalar-t 'bool))) args)
                                             (if (eq? op 'and) " && " " || ")))]
       [(not) (format "(!~a)" (cpp-expr (first args) env (scalar-t 'bool)))]
       [(= < <= > >=)
        (define operand (resolve (unify (type-of (first args)) (type-of (second args))) #f))
        (format "(~a ~a ~a)" (cpp-expr (first args) env operand) (if (eq? op '=) "==" op)
                (cpp-expr (second args) env operand))]
       [(+ - * /)
        (if (= (length args) 1)
            (format "(-~a)" (cpp-expr (first args) env t))
            (format "(~a)" (string-join (map (λ (a) (cpp-expr a env t)) args) (format " ~a " op))))]
       [(min max) (format "std::~a(~a, ~a)" op (cpp-expr (first args) env t) (cpp-expr (second args) env t))]
       [(float int uint)
        (format "static_cast<~a>(~a)" (value-cpp-type (scalar-t op)) (cpp-expr (first args) env #f))]
       [(width height)
        (define image (e-ref-name (first args)))
        (match (hash-ref env image)
          [(entry 'import _ _) (format "zgal::extentOf(context, in->~a, ~a).~a" image (cpp-string image) op)]
          [(entry 'image _ item)
           (define id (format "image_~a" image))
           (format "graph.imageExtent(~a, frame.frameIndex).~a"
                   (if (g-image-when item) (format "zgal::require(~a, ~a, ~a)" id (cpp-string (current-graph-name)) (cpp-string image)) id)
                   op)])]
       [else
        (define component (scalar-t (vector-t-kind t)))
        (format "~a(~a)" (cpp-type t) (string-join (map (λ (a) (cpp-expr a env component)) args) ", "))])]))

;; ---------------------------------------------------------------------------------------------
;; Choices

(define (vk-enum prefix sym)
  (string-append prefix (string-upcase (string-replace (symbol->string sym) "-" "_"))))

(define (compare-op sym) (vk-enum "VK_COMPARE_OP_" sym))

(define (border-color sym)
  (case sym
    [(transparent-black) "VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK"]
    [(opaque-black) "VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK"]
    [(opaque-white) "VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE"]))

(define (blend-mode sym)
  (case sym
    [(opaque) "BlendMode::Opaque"] [(alpha) "BlendMode::Alpha"]
    [(premultiplied-alpha) "BlendMode::PremultipliedAlpha"] [(additive) "BlendMode::Additive"]))

(define (cull-mode sym)
  (case sym
    [(none) "VK_CULL_MODE_NONE"] [(back) "VK_CULL_MODE_BACK_BIT"] [(front) "VK_CULL_MODE_FRONT_BIT"]
    [(front-and-back) "VK_CULL_MODE_FRONT_AND_BACK"]))

(define (resting-state sym)
  (case sym
    [(shader-read-only) "ImageState::shaderReadOnly()"] [(general) "ImageState::general()"]
    [(transfer-src) "ImageState::transferSrc()"]))

(define (frame-mode sym) (if (eq? sym 'shared) "FrameMode::Shared" "FrameMode::PerFrame"))

(define (color->cpp c) (format "{~a}" (string-join (map (λ (x) (literal->cpp x (scalar-t 'float))) c) ", ")))

;; ---------------------------------------------------------------------------------------------
;; The graph

;; The members of g's Inputs struct, in order: (cons item declaration)
(define (graph-input-members g)
  (define name (graph-name g))
  (for/list ([item (in-list (graph-items g))]
             #:when (or (g-parameter? item) (g-input? item) (g-import? item) (g-import-buffer? item)))
    (cons item
          (match item
            [(g-parameter n t _) (format "~a ~a{};" (value-cpp-type t) n)]
            [(g-input n kind t _)
             (format "~a ~a;"
                     (match kind
                       ['per-frame (format "zgal::FrameSource<~a>" (value-cpp-type t))]
                       ['meshes "std::vector<Handle<Mesh>>"]
                       ['cpu "RenderGraph::RunFn"]
                       ['extern (format "zgal::ExternNodes<~a>" name)])
                     n)]
            [(g-import n resting _) (format "FrameBinding<Image> ~a;   // Imported, resting ~a" n (symbol->string resting))]
            [(g-import-buffer n t _) (format "FrameBinding<Buffer<~a>> ~a;   // Imported" (cpp-type t) n)]))))

(define (emit-graph! w prog g)
  (parameterize ([current-graph-name (graph-name g)])
    (emit-graph-body! w prog g)))

(define (emit-graph-body! w prog g)
  (define name (graph-name g))
  (define env (graph-environment g))
  (define items (graph-items g))
  (define nodes (graph-nodes g))
  (define (gen text) (emit! w text))
  (define (at item text) (emit! w text #:at (item-loc item)))
  (define (of-type pred) (filter pred items))
  (define (conditional? n)
    (match (hash-ref env n #f)
      [(entry (or 'image 'buffer) _ item) (and (if (g-image? item) (g-image-when item) (g-buffer-when item)) #t)]
      [_ #f]))
  ;; An image's or buffer's id, checked to exist if it's conditional
  (define (resource-ref n)
    (define variable (format "~a_~a" (if (memq (entry-kind (hash-ref env n)) '(image import)) "image" "buffer") n))
    (if (conditional? n)
        (format "zgal::require(~a, ~a, ~a)" variable (cpp-string name) (cpp-string n))
        variable))
  (define (build-expr e want) (cpp-expr e env want))

  (define parameters (of-type g-parameter?))
  (define inputs (of-type g-input?))
  (define imports (of-type g-import?))
  (define buffer-imports (of-type g-import-buffer?))
  (define images (of-type g-image?))
  (define buffers (of-type g-buffer?))
  (define materials (of-type g-material?))

  (gen "")
  (gen (format "// graph ~a: build() adds its resources, materials and nodes to a RenderGraph" name))
  (emit! w (format "struct ~a {" name) #:at (graph-loc g))
  ;; Inputs
  (gen "    // What C++ supplies: the parameters, and the inputs. Give every one.")
  (gen "    struct Inputs {")
  (for ([m (in-list (graph-input-members g))])
    (at (car m) (string-append "        " (cdr m))))
  (gen "    };")
  (gen "")
  ;; Ids
  (define (id-struct title members)
    (unless (null? members)
      (gen (format "    struct ~a {" title))
      (for ([m (in-list members)])
        (match-define (list type member-name item) m)
        (at item (format "        ~a ~a;" type member-name)))
      (gen "    };")))
  (define (id-type base conditional?) (if conditional? (format "std::optional<~a>" base) base))
  (id-struct "Images"
             (append (for/list ([i (in-list imports)]) (list "ImageId" (g-import-name i) i))
                     (for/list ([i (in-list images)]) (list (id-type "ImageId" (g-image-when i)) (g-image-name i) i))))
  (id-struct "Buffers"
             (append (for/list ([b (in-list buffer-imports)]) (list "BufferId" (g-import-buffer-name b) b))
                     (for/list ([b (in-list buffers)]) (list (id-type "BufferId" (g-buffer-when b)) (g-buffer-name b) b))))
  (id-struct "Nodes"
             (for/list ([n (in-list nodes)])
               ;; An extern node is the nodes C++ adds (none when its condition is false)
               (list (if (eq? (g-node-kind n) 'extern) "std::vector<NodeId>" (id-type "NodeId" (g-node-when n)))
                     (g-node-name n) n)))
  (unless (and (null? imports) (null? images)) (gen "    Images images;"))
  (unless (and (null? buffer-imports) (null? buffers)) (gen "    Buffers buffers;"))
  (unless (null? nodes) (gen "    Nodes nodes;"))
  (gen "")
  (gen "    // The device features the graph's materials need beyond the wrapper's: add them to the")
  (gen "    // context's (VulkanContextConfig::features, AppConfig::features) before creating it")
  (gen "    static DeviceFeatures requiredFeatures() {")
  (gen "        DeviceFeatures features;")
  (for ([type (in-list (remove-duplicates (map (λ (mi) (material-name (g-material-material mi))) materials)))])
    (gen (format "        features.add(~a::requiredFeatures());" type)))
  (gen "        return features;")
  (gen "    }")
  (gen "")
  (gen "    // Adds the graph's resources, materials and nodes to `graph`. Its materials and nodes keep")
  (gen "    // the inputs' callbacks, so whatever those use must outlive the graph.")
  (gen (format "    static Result<~a> build(VulkanContext& context, RenderGraph& graph, Inputs inputs) {" name))
  (gen "        const auto in = std::make_shared<const Inputs>(std::move(inputs));")
  (for ([i (in-list inputs)] #:unless (eq? (g-input-kind i) 'meshes))
    (at i (format "        if (!in->~a) {" (g-input-name i)))
    (at i (format "            contractViolation(~a);" (cpp-string (format "~a's input ~a isn't given" name (g-input-name i)))))
    (at i "        }"))
  (for ([i (in-list (append imports buffer-imports))])
    (define-values (n what) (if (g-import? i) (values (g-import-name i) "image") (values (g-import-buffer-name i) "buffer")))
    (at i (format "        if (!in->~a.isPerFrame() && !in->~a.shared()) {" n n))
    (at i (format "            contractViolation(~a);" (cpp-string (format "~a's imported ~a ~a isn't given" name what n))))
    (at i "        }"))
  (gen (format "        ~a result;" name))

  ;; Imported images
  (for ([i (in-list imports)])
    (at i (format "        const ImageId image_~a = graph.importImage(~a, in->~a, ~a);" (g-import-name i)
                  (cpp-string (g-import-name i)) (g-import-name i) (resting-state (g-import-resting i))))
    (gen (format "        result.images.~a = image_~a;" (g-import-name i) (g-import-name i))))
  ;; Imported buffers
  (for ([i (in-list buffer-imports)])
    (define n (g-import-buffer-name i))
    (at i (format "        const BufferId buffer_~a = graph.importBuffer<~a>(~a, in->~a);" n
                  (cpp-type (g-import-buffer-type i)) (cpp-string n) n))
    (gen (format "        result.buffers.~a = buffer_~a;" n n)))
  ;; Constants
  (for ([c (in-list (of-type g-constant?))])
    (at c (format "        const ~a constant_~a = ~a;" (value-cpp-type (g-constant-type c)) (g-constant-name c)
                  (build-expr (g-constant-expr c) (g-constant-type c)))))
  ;; Samplers
  (for ([s (in-list (of-type g-sampler?))])
    (define o (g-sampler-options s))
    (define (opt k) (cond [(assq k o) => cdr] [else #f]))
    (define fields
      (filter values
              (list (and (opt 'filter) (format ".filter = ~a" (vk-enum "VK_FILTER_" (opt 'filter))))
                    (and (opt 'mipmap) (format ".mipmapMode = ~a" (vk-enum "VK_SAMPLER_MIPMAP_MODE_" (opt 'mipmap))))
                    (and (opt 'address) (format ".addressMode = ~a" (vk-enum "VK_SAMPLER_ADDRESS_MODE_" (opt 'address))))
                    (and (opt 'border) (format ".borderColor = ~a" (border-color (opt 'border))))
                    (and (opt 'compare) (format ".compareOp = ~a" (compare-op (opt 'compare))))
                    (and (opt 'anisotropy) (format ".anisotropy = ~a" (if (eq? (opt 'anisotropy) 'yes) "true" "false")))
                    (and (opt 'max-lod) (format ".maxLod = ~a" (literal->cpp (opt 'max-lod) (scalar-t 'float)))))))
    (at s (format "        TRY_ASSIGN(auto sampler_~a, VulkanSampler::create(context, SamplerDesc{~a}));"
                  (g-sampler-name s) (string-join fields ", "))))
  ;; Uniforms
  (for ([u (in-list (of-type g-uniform?))])
    (at u (format "        TRY_ASSIGN(auto uniform_~a, zgal::Uniform<~a>::create(context, in->~a));"
                  (g-uniform-name u) (cpp-type (g-uniform-type u)) (g-uniform-source u))))
  ;; Images and buffers
  (for ([item (in-list items)] #:when (or (g-image? item) (g-buffer? item)))
    (define-values (variable kind-name when create)
      (match item
        [(g-image n f extent frames when _)
         (values (format "image_~a" n) "images" when
                 (format "graph.createImage(~a, ~a, ~a, ~a)" (cpp-string n) (frame-mode frames) (hash-ref image-formats f)
                         (match extent
                           [(list 'fixed w h) (format "FixedExtent{{~a, ~a}}" (build-expr w (scalar-t 'uint)) (build-expr h (scalar-t 'uint)))]
                           [(list 'surface scale) (format "SurfaceRelative{~a}" (literal->cpp scale (scalar-t 'float)))]
                           [(list 'extent-of import)
                            (format "FixedExtent{zgal::extentOf(context, in->~a, ~a)}" import (cpp-string import))])))]
        [(g-buffer n t count frames when _)
         (values (format "buffer_~a" n) "buffers" when
                 (format "graph.createBuffer(~a, ~a, VkDeviceSize(~a) * sizeof(~a))" (cpp-string n) (frame-mode frames)
                         (build-expr count (scalar-t 'uint)) (cpp-type t)))]))
    (define id-type-name (if (g-image? item) "ImageId" "BufferId"))
    (cond
      [when
       (at item (format "        std::optional<~a> ~a;" id-type-name variable))
       (at item (format "        if (~a) {" (build-expr when (scalar-t 'bool))))
       (at item (format "            ~a = ~a;" variable create))
       (gen "        }")]
      [else (at item (format "        const ~a ~a = ~a;" id-type-name variable create))])
    (gen (format "        result.~a.~a = ~a;" kind-name (graph-item-name item) variable)))
  ;; Targets
  (for ([t (in-list (of-type g-target?))])
    (define variable (format "target_~a" (g-target-name t)))
    (at t (format "        RenderTarget ~a;" variable))
    (let emit-spec ([spec (g-target-spec t)] [indent "        "])
      (define (line text) (emit! w (string-append indent text) #:at (g-target-loc t)))
      (match spec
        [(t-attachments colors depth _)
         (for ([c (in-list colors)])
           (line (format "~a.color.push_back({~a, ~a, ~a, ~a});" variable (resource-ref (t-attachment-image c))
                         (vk-enum "VK_ATTACHMENT_LOAD_OP_" (t-attachment-load c))
                         (vk-enum "VK_ATTACHMENT_STORE_OP_" (t-attachment-store c)) (color->cpp (t-attachment-clear c)))))
         (when depth
           (line (format "~a.depth = DepthAttachment{~a, ~a, ~a, ~a};" variable (resource-ref (t-attachment-image depth))
                         (vk-enum "VK_ATTACHMENT_LOAD_OP_" (t-attachment-load depth))
                         (vk-enum "VK_ATTACHMENT_STORE_OP_" (t-attachment-store depth))
                         (literal->cpp (t-attachment-clear depth) (scalar-t 'float)))))]
        [(t-surface clear depth _)
         (line (format "~a = graph.surfaceTarget(~a, ~a);" variable (color->cpp clear)
                       (if (eq? depth 'none) "std::nullopt" (hash-ref image-formats depth))))]
        [(t-target-if c then else _)
         (line (format "if (~a) {" (build-expr c (scalar-t 'bool))))
         (emit-spec then (string-append indent "    "))
         (line "} else {")
         (emit-spec else (string-append indent "    "))
         (line "}")])))
  ;; Materials
  (for ([mi (in-list materials)])
    (define m (g-material-material mi))
    (define type (material-name m))
    (define instance (g-material-name mi))
    (define graphics? (eq? (material-kind m) 'graphics))
    (define (line text) (emit! w text #:at (g-material-loc mi)))
    (define updates
      (for/list ([pair (in-list (g-material-bindings mi))]
                 #:when (eq? (entry-kind (hash-ref env (cdr pair))) 'uniform))
        (format "[uniform = uniform_~a](const zgal::FrameInfo& frame) { uniform->update(frame); }" (cdr pair))))
    (define entries
      (for/list ([pair (in-list (g-material-bindings mi))])
        (match-define (cons b resource) pair)
        (format ".~a = ~a" (binding-name b)
                (match* ((binding-kind b) (entry-kind (hash-ref env resource)))
                  [('texture _) (format "graph.sampledEntry(~a)" (resource-ref resource))]
                  [('storage-image _) (format "graph.storageEntry(~a)" (resource-ref resource))]
                  [('sampler _) (format "context.descriptors().sampler(sampler_~a)" resource)]
                  [(_ 'uniform) (format "uniform_~a->entries()" resource)]
                  [(_ (or 'buffer 'import-buffer)) (format "graph.storageBufferEntry(~a)" (resource-ref resource))]))))
    (define target
      (for/first ([n (in-list nodes)] #:when (and (equal? (g-node-material n) instance) (g-node-target n)))
        (g-node-target n)))
    (define options (g-material-options mi))
    (define (option k) (cond [(assq k options) => cdr] [else #f]))
    (define pipeline-fields
      (filter values
              (list (and (option 'topology) (format ".topology = ~a" (vk-enum "VK_PRIMITIVE_TOPOLOGY_" (option 'topology))))
                    (and (option 'cull) (format ".cullMode = ~a" (cull-mode (option 'cull))))
                    (and (option 'front-face) (format ".frontFace = ~a" (vk-enum "VK_FRONT_FACE_" (option 'front-face))))
                    (and (option 'blend) (format ".blend = {~a}" (blend-mode (option 'blend))))
                    (and (option 'depth-test) (format ".depthTest = ~a" (if (eq? (option 'depth-test) 'yes) "true" "false")))
                    (and (option 'depth-write) (format ".depthWrite = ~a" (if (eq? (option 'depth-write) 'yes) "true" "false")))
                    (and (option 'depth-compare) (format ".depthCompare = ~a" (compare-op (option 'depth-compare)))))))
    ;; Graphics materials don't dispatch; compute materials' workgroups are computed each frame
    (define dispatch
      (if graphics?
          "nullptr"
          (format "[~a](const zgal::FrameInfo& frame) { return ~a::workgroupsFor(~a); }"
                  (frame-captures (g-material-invocations mi) env) type
                  (string-join (for/list ([e (in-list (g-material-invocations mi))]) (build-expr e (scalar-t 'uint))) ", "))))
    (define push? (pair? (g-material-push mi)))
    (line (format "        TRY_ASSIGN(auto shaders_~a, ~a::loadShaders(context));" instance type))
    (line (format "        auto material_~a = context.resources().create<zgal::~a<~a>>(" instance (if push? "PushInstance" "Instance") type))
    (line (format "            zgal::Updates{~a}," (string-join updates ", ")))
    (when push?
      (line (format "            [~a](const zgal::FrameInfo& frame, ~a::Push& push) {"
                    (frame-captures (map cdr (g-material-push mi)) env) type))
      (for ([pair (in-list (g-material-push mi))])
        (match-define (cons f e) pair)
        (emit! w (format "                push.~a = ~a;" (field-name f) (cpp-expr e env (field-type f))) #:at (field-loc f)))
      (line "            },"))
    (line (format "            ~a, context, shaders_~a~a~a);" dispatch instance
                  (if (null? entries) "" (format ", ~a::Bindings{~a}" type (string-join entries ", ")))
                  (if graphics?
                      (format ", graph.formats(target_~a), GraphicsPipelineOptions{~a}" target (string-join pipeline-fields ", "))
                      ""))))
  ;; Nodes
  (for ([n (in-list nodes)])
    (define (line text) (emit! w text #:at (g-node-loc n)))
    (define node-name (g-node-name n))
    (define create
      (match (g-node-kind n)
        ['dispatch (format "graph.add<DispatchNode>(~a, material_~a)" (cpp-string node-name) (g-node-material n))]
        ['draw-meshes (format "graph.add<RenderablesNode>(~a, std::move(renderables), target_~a)" (cpp-string node-name) (g-node-target n))]
        ['draw
         (format "graph.addGraphics(~a, [target = target_~a, material = material_~a](NodeBuilder& builder) { builder.renderTo(target).uses(material); }, [material = material_~a, count = ~a](RecordContext& record) { material->bind(record.commandBuffer, record.frameIndex); vkCmdDraw(record.commandBuffer, count, 1, 0, 0); })"
                 (cpp-string node-name) (g-node-target n) (g-node-material n) (g-node-material n)
                 (build-expr (g-node-count n) (scalar-t 'uint)))]
        ['readback (format "graph.add<ReadbackNode>(~a, ~a)" (cpp-string node-name) (resource-ref (g-node-subject n)))]
        ['cpu
         (define uses
           (append (for/list ([b (in-list (g-node-reads n))]) (format ".read(~a, Access::HostRead)" (resource-ref b)))
                   (for/list ([b (in-list (g-node-writes n))]) (format ".write(~a, Access::HostWrite)" (resource-ref b)))))
         (format "graph.addCpu(~a, ~a, in->~a)" (cpp-string node-name)
                 (if (null? uses)
                     "nullptr"
                     (format "[~a](NodeBuilder& builder) { builder~a; }"
                             (string-join (for/list ([b (in-list (remove-duplicates (append (g-node-reads n) (g-node-writes n))))])
                                            (format "buffer_~a" b))
                                          ", ")
                             (string-join uses "")))
                 (g-node-subject n))]
        ['copy
         (define source (first (g-node-reads n)))
         (define destination (first (g-node-writes n)))
         (define element (cpp-type (entry-type (hash-ref env source))))
         (format "graph.addTransfer(~a, [source = ~a, destination = ~a](NodeBuilder& builder) { builder.read(source, Access::TransferSrc).write(destination, Access::TransferDst); }, [source = ~a, destination = ~a, sourceOffset = VkDeviceSize(~a), destinationOffset = VkDeviceSize(~a), count = ~a](RecordContext& record) { zgal::copyBuffer(record, source, destination, sizeof(~a), sourceOffset, destinationOffset, count, ~a); })"
                 (cpp-string node-name) (resource-ref source) (resource-ref destination) (resource-ref source) (resource-ref destination)
                 (build-expr (car (g-node-offsets n)) (scalar-t 'uint)) (build-expr (cdr (g-node-offsets n)) (scalar-t 'uint))
                 (if (g-node-count n) (format "std::optional<VkDeviceSize>(~a)" (build-expr (g-node-count n) (scalar-t 'uint))) "std::optional<VkDeviceSize>()")
                 element (cpp-string node-name))]
        ['extern #f]))
    (line (if (g-node-when n)
              (format "        if (~a) {" (build-expr (g-node-when n) (scalar-t 'bool)))
              "        {"))
    (when (eq? (g-node-kind n) 'draw-meshes)
      (line "            std::vector<Handle<Renderable>> renderables;")
      (line (format "            for (const auto& mesh : in->~a) {" (g-node-subject n)))
      (line (format "                renderables.push_back(context.resources().create<MeshRenderable>(mesh, material_~a));" (g-node-material n)))
      (line "            }"))
    (cond
      [(eq? (g-node-kind n) 'extern)
       ;; C++ adds the nodes, with the ids so far (its nodes may use the graph's resources)
       (line (format "            result.nodes.~a = in->~a(graph, result);" node-name (g-node-subject n)))]
      [else
       (line (format "            const NodeId node = ~a;" create))
       (when (g-node-enabled n)
         (line (format "            graph.setEnabled(node, ~a);" (build-expr (g-node-enabled n) (scalar-t 'bool)))))
       (line (format "            result.nodes.~a = node;" node-name))])
    (line "        }"))
  (gen "        return result;")
  (gen "    }")
  (emit! w "};" #:at (graph-loc g)))

(define (item-loc item)
  (match item
    [(g-parameter _ _ l) l] [(g-input _ _ _ l) l] [(g-import _ _ l) l] [(g-import-buffer _ _ l) l] [(g-constant _ _ _ l) l]
    [(g-sampler _ _ l) l] [(g-uniform _ _ _ l) l] [(g-image _ _ _ _ _ l) l] [(g-buffer _ _ _ _ _ l) l]
    [(g-target _ _ l) l] [(g-material _ _ _ _ _ _ l) l] [(g-node _ _ _ _ _ _ _ _ _ _ _ l) l]))
