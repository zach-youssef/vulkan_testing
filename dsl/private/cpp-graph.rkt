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

;; Whether an item is one a node declared for itself (e.g. a kernel's scratch buffer), which the
;; generated struct's ids leave out
(define (internal-name? n) (string-prefix? n "zgal_"))

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
       [(count)
        (define input (e-ref-name (first args)))
        (if (eq? (entry-kind (hash-ref env input)) 'per-frame-data)
            (format "data_~a->get(frame).size()" input)
            (format "static_cast<uint32_t>(in->~a.size())" input))]
       [(float int uint)
        (format "static_cast<~a>(~a)" (value-cpp-type (scalar-t op)) (cpp-expr (first args) env #f))]
       [(width height)
        (define image (e-ref-name (first args)))
        (match (hash-ref env image)
          [(entry 'import _ item) (format "zgal::extentOf(context, ~a, ~a).~a" (imported-image item) (cpp-string image) op)]
          [(entry 'image _ item)
           (define id (format "image_~a" image))
           (format "graph.imageExtent(~a, frame.frameIndex).~a"
                   (if (g-image-when item) (format "zgal::require(~a, ~a, ~a)" id (cpp-string (current-graph-name)) (cpp-string image)) id)
                   op)])]
       [else
        (define component (scalar-t (vector-t-kind t)))
        (format "~a(~a)" (cpp-type t) (string-join (map (λ (a) (cpp-expr a env component)) args) ", "))])]))

;; An asset's path in C++: a string literal or a path input
(define (asset-path source)
  (if (asset-source-path source)
      (format "std::filesystem::path(~a)" (cpp-string (asset-source-path source)))
      (format "in->~a" (asset-source-input source))))

;; An imported image's binding: the input C++ gives, or the image a file was loaded into
(define (imported-image item)
  (if (g-image-file? item)
      (format "file_~a" (g-image-file-name item))
      (format "in->~a" (graph-item-name item))))

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
             #:when (or (g-parameter? item) (g-input? item) (g-import? item) (g-import-images? item) (g-import-buffer? item)))
    (cons item
          (match item
            [(g-parameter n t _) (format "~a ~a{};" (value-cpp-type t) n)]
            [(g-input n kind t _)
             (format "~a ~a;"
                     (match kind
                       ['per-frame (format "zgal::FrameSource<~a>" (value-cpp-type t))]
                       ['data (format "std::vector<~a>" (cpp-type t))]
                       ['per-frame-data (format "zgal::FrameSource<zgal::Data<~a>>" (cpp-type t))]
                       ['meshes "std::vector<Handle<Mesh>>"]
                       ['cpu "RenderGraph::RunFn"]
                       ['extern (format "zgal::ExternNodes<~a>" name)]
                       ['path "std::filesystem::path"])
                     n)]
            [(g-import n resting _) (format "FrameBinding<Image> ~a;   // Imported, resting ~a" n (symbol->string resting))]
            [(g-import-images n resting _) (format "std::vector<FrameBinding<Image>> ~a;   // Imported, resting ~a" n (symbol->string resting))]
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
  (define (image-kind? resource) (memq (entry-kind (hash-ref env resource)) '(image import)))

  (define parameters (of-type g-parameter?))
  (define inputs (of-type g-input?))
  (define imports (of-type g-import?))
  (define image-files (of-type g-image-file?))
  (define model-textures (of-type g-model-textures?))
  (define model-factors (of-type g-model-factors?))
  (define model-draws (of-type g-model-draws?))
  (define model-bounds (of-type g-model-bounds?))
  (define image-sets (of-type g-import-images?))
  (define buffer-imports (of-type g-import-buffer?))
  (define uploads (of-type g-upload?))
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
  (define (id-struct title all-members)
    (define members (filter (λ (m) (not (internal-name? (second m)))) all-members))
    (unless (null? members)
      (gen (format "    struct ~a {" title))
      (for ([m (in-list members)])
        (match-define (list type member-name item) m)
        (at item (format "        ~a ~a;" type member-name)))
      (gen "    };")))
  (define (id-type base conditional?) (if conditional? (format "std::optional<~a>" base) base))
  (id-struct "Images"
             (append (for/list ([i (in-list imports)]) (list "ImageId" (g-import-name i) i))
                     (for/list ([i (in-list image-files)]) (list "ImageId" (g-image-file-name i) i))
                     (for/list ([i (in-list image-sets)]) (list "std::vector<ImageId>" (g-import-images-name i) i))
                     (for/list ([i (in-list model-textures)]) (list "std::vector<ImageId>" (g-model-textures-name i) i))
                     (for/list ([i (in-list images)]) (list (id-type "ImageId" (g-image-when i)) (g-image-name i) i))))
  (id-struct "Buffers"
             (append (for/list ([b (in-list buffer-imports)]) (list "BufferId" (g-import-buffer-name b) b))
                     (for/list ([b (in-list uploads)]) (list "BufferId" (g-upload-name b) b))
                     (for/list ([b (in-list model-factors)]) (list "BufferId" (g-model-factors-name b) b))
                     (for/list ([b (in-list model-draws)]) (list "BufferId" (g-model-draws-name b) b))
                     (for/list ([b (in-list model-bounds)]) (list "BufferId" (g-model-bounds-name b) b))
                     (for/list ([b (in-list buffers)]) (list (id-type "BufferId" (g-buffer-when b)) (g-buffer-name b) b))))
  (id-struct "Nodes"
             (for/list ([n (in-list nodes)])
               ;; An extern node is the nodes C++ adds (none when its condition is false)
               (list (if (eq? (g-node-kind n) 'extern) "std::vector<NodeId>" (id-type "NodeId" (g-node-when n)))
                     (g-node-name n) n)))
  (unless (and (null? imports) (null? image-files) (null? image-sets) (null? model-textures) (null? images)) (gen "    Images images;"))
  ;; Readbacks' results, typed
  (define readbacks (filter (λ (n) (eq? (g-node-kind n) 'readback)) nodes))
  (unless (null? readbacks)
    (gen ""))
  (for ([n (in-list readbacks)])
    (define subject (g-node-subject n))
    (define id (if (g-node-when n)
                   (format "zgal::require(nodes.~a, ~a, ~a)" (g-node-name n) (cpp-string name) (cpp-string (g-node-name n)))
                   (format "nodes.~a" (g-node-name n))))
    (cond
      [(image-kind? subject)
       (at n (format "    // The pixels readback ~a copied in the most recent frame (waiting for it)" (g-node-name n)))
       (at n (format "    Result<std::vector<uint8_t>> ~aPixels(RenderGraph& graph) const { return graph.nodeAs<ReadbackNode>(~a).pixels(); }"
                     (g-node-name n) id))]
      [else
       (define t (cpp-type (entry-type (hash-ref env subject))))
       (at n (format "    // The elements readback ~a copied in the most recent frame (waiting for it)" (g-node-name n)))
       (at n (format "    Result<std::vector<~a>> ~aContents(RenderGraph& graph) const { return graph.nodeAs<BufferReadbackNode>(~a).contents<~a>(); }"
                     t (g-node-name n) id t))]))
  (unless (and (null? buffer-imports) (null? uploads) (null? model-factors) (null? model-draws) (null? model-bounds) (null? (filter (λ (b) (not (internal-name? (g-buffer-name b)))) buffers)))
    (gen "    Buffers buffers;"))
  (unless (null? nodes) (gen "    Nodes nodes;"))
  (gen "")
  (gen "    // The device features the graph's materials need beyond the wrapper's: add them to the")
  (gen "    // context's (VulkanContextConfig::features, AppConfig::features) before creating it")
  (gen "    static DeviceFeatures requiredFeatures() {")
  (gen "        DeviceFeatures features;")
  (for ([type (in-list (remove-duplicates (map (λ (mi) (material-name (g-material-material mi))) materials)))])
    (gen (format "        features.add(~a::requiredFeatures());" type)))
  ;; Indirect draws of more than one command
  (when (for/or ([n (in-list nodes)])
          (match (g-node-extra n)
            [(indirect _ draws _ _ _) (and draws (not (equal? (e-literal-value* draws) 1)))]
            [_ #f]))
    (gen "        features.core.features.multiDrawIndirect = VK_TRUE;   // Indirect draws of several commands"))
  ;; Indirect draws of models' meshes, whose commands' first instances are materials
  (when (for/or ([n (in-list nodes)]) (and (eq? (g-node-kind n) 'draw-meshes) (indirect? (g-node-extra n))))
    (gen "        features.core.features.drawIndirectFirstInstance = VK_TRUE;   // Models' draws, whose first instances are their materials"))
  (gen "        return features;")
  (gen "    }")
  (gen "")
  (gen "    // Adds the graph's resources, materials and nodes to `graph`. Its materials and nodes keep")
  (gen "    // the inputs' callbacks, so whatever those use must outlive the graph.")
  (gen (format "    static Result<~a> build(VulkanContext& context, RenderGraph& graph, Inputs inputs) {" name))
  (gen "        const auto in = std::make_shared<const Inputs>(std::move(inputs));")
  ;; Functions must be given; meshes and data may be empty (an upload checks its own), and paths are
  ;; checked when they're loaded
  (for ([i (in-list inputs)] #:unless (memq (g-input-kind i) '(meshes data path)))
    (at i (format "        if (!in->~a) {" (g-input-name i)))
    (at i (format "            contractViolation(~a);" (cpp-string (format "~a's input ~a isn't given" name (g-input-name i)))))
    (at i "        }"))
  (for ([i (in-list (append imports buffer-imports))])
    (define-values (n what) (if (g-import? i) (values (g-import-name i) "image") (values (g-import-buffer-name i) "buffer")))
    (at i (format "        if (!in->~a.isPerFrame() && !in->~a.shared()) {" n n))
    (at i (format "            contractViolation(~a);" (cpp-string (format "~a's imported ~a ~a isn't given" name what n))))
    (at i "        }"))
  (gen (format "        ~a result;" name))
  ;; Per-frame data, computed once a frame however many use it
  (for ([i (in-list inputs)] #:when (eq? (g-input-kind i) 'per-frame-data))
    (at i (format "        const auto data_~a = std::make_shared<zgal::FrameCache<zgal::Data<~a>>>(in->~a);"
                  (g-input-name i) (cpp-type (g-input-type i)) (g-input-name i))))

  ;; Imported images
  (for ([i (in-list imports)])
    (at i (format "        const ImageId image_~a = graph.importImage(~a, in->~a, ~a);" (g-import-name i)
                  (cpp-string (g-import-name i)) (g-import-name i) (resting-state (g-import-resting i))))
    (gen (format "        result.images.~a = image_~a;" (g-import-name i) (g-import-name i))))
  ;; Image files, loaded and imported
  (for ([i (in-list image-files)])
    (define n (g-image-file-name i))
    (at i (format "        TRY_ASSIGN(auto file_~a, zgal::loadImage(context, ~a, ~a, ~a));" n (asset-path (g-image-file-source i))
                  (if (eq? (g-image-file-color i) 'srgb) "ColorSpace::Srgb" "ColorSpace::Linear")
                  (cpp-string (format "~a's image-file ~a" name n))))
    (at i (format "        const ImageId image_~a = graph.importImage(~a, file_~a, ImageState::shaderReadOnly());" n (cpp-string n) n))
    (gen (format "        result.images.~a = image_~a;" n n)))
  ;; Models: loaded and uploaded, then their materials' textures imported and factors uploaded
  (for ([item (in-list items)] #:when (or (g-model? item) (g-model-textures? item) (g-model-factors? item) (g-model-draws? item) (g-model-bounds? item)))
    (match item
      [(g-model n source f _)
       (at item (format "        TRY_ASSIGN(auto model_~a, zgal::loadModel<~a>(context, ~a, ~a));" n (vertex-format-name f)
                        (asset-path source) (cpp-string (format "~a's model ~a" name n))))]
      [(g-model-textures n model _)
       (at item (format "        const std::vector<ImageId> images_~a = zgal::importImages(graph, ~a, model_~a.baseColorTextures, ImageState::shaderReadOnly());"
                        n (cpp-string n) model))
       (gen (format "        result.images.~a = images_~a;" n n))]
      [(g-model-factors n model _)
       (at item (format "        TRY_ASSIGN(auto upload_~a, zgal::upload(context, model_~a.baseColorFactors, ~a));" n model
                        (cpp-string (format "~a's base colour factors ~a" name n))))
       (at item (format "        const BufferId buffer_~a = graph.importBuffer<glm::vec4>(~a, upload_~a);" n (cpp-string n) n))
       (gen (format "        result.buffers.~a = buffer_~a;" n n))]
      [(g-model-draws n model t _)
       (at item (format "        TRY_ASSIGN(auto upload_~a, zgal::uploadAs<~a>(context, model_~a.draws, ~a));" n (cpp-type t) model
                        (cpp-string (format "~a's model draws ~a" name n))))
       (at item (format "        const BufferId buffer_~a = graph.importBuffer<~a>(~a, upload_~a);" n (cpp-type t) (cpp-string n) n))
       (gen (format "        result.buffers.~a = buffer_~a;" n n))]
      [(g-model-bounds n model _)
       (at item (format "        TRY_ASSIGN(auto upload_~a, zgal::upload(context, model_~a.bounds, ~a));" n model
                        (cpp-string (format "~a's model bounds ~a" name n))))
       (at item (format "        const BufferId buffer_~a = graph.importBuffer<glm::vec4>(~a, upload_~a);" n (cpp-string n) n))
       (gen (format "        result.buffers.~a = buffer_~a;" n n))]))
  ;; Imported sets of images, each imported
  (for ([i (in-list image-sets)])
    (define n (g-import-images-name i))
    (at i (format "        std::vector<ImageId> images_~a;" n))
    (at i (format "        for (size_t i = 0; i < in->~a.size(); ++i) {" n))
    (at i (format "            if (!in->~a[i].isPerFrame() && !in->~a[i].shared()) {" n n))
    (at i (format "                contractViolation(~a + std::to_string(i) + \" isn't given\");"
                  (cpp-string (format "~a's imported image ~a " name n))))
    (at i "            }")
    (at i (format "            images_~a.push_back(graph.importImage(~a + std::to_string(i), in->~a[i], ~a));" n
                  (cpp-string (format "~a " n)) n (resting-state (g-import-images-resting i))))
    (at i "        }")
    (gen (format "        result.images.~a = images_~a;" n n)))
  ;; Imported buffers
  (for ([i (in-list buffer-imports)])
    (define n (g-import-buffer-name i))
    (at i (format "        const BufferId buffer_~a = graph.importBuffer<~a>(~a, in->~a);" n
                  (cpp-type (g-import-buffer-type i)) (cpp-string n) n))
    (gen (format "        result.buffers.~a = buffer_~a;" n n)))
  ;; Uploaded buffers: created from their data inputs, then imported; or for per-frame data, a buffer
  ;; per frame in flight sized from the data, which a node fills when it changes
  (for ([u (in-list uploads)] #:when (eq? (entry-kind (hash-ref env (g-upload-source u))) 'per-frame-data))
    (define n (g-upload-name u))
    (define t (cpp-type (g-upload-type u)))
    (at u (format "        const BufferId buffer_~a = graph.createBuffer(~a, FrameMode::PerFrame, [data = data_~a](const FrameContext& frameContext) { return VkDeviceSize(data->get(zgal::frameInfo(frameContext)).size()) * sizeof(~a); });"
                  n (cpp-string n) (g-upload-source u) t))
    (at u (format "        graph.add<zgal::DataUploadNode<~a>>(~a, buffer_~a, data_~a);" t (cpp-string (format "upload ~a" n)) n (g-upload-source u)))
    (gen (format "        result.buffers.~a = buffer_~a;" n n)))
  (for ([u (in-list uploads)] #:unless (eq? (entry-kind (hash-ref env (g-upload-source u))) 'per-frame-data))
    (define n (g-upload-name u))
    (at u (format "        TRY_ASSIGN(auto upload_~a, zgal::upload(context, in->~a, ~a));" n (g-upload-source u)
                  (cpp-string (format "~a's upload ~a" name n))))
    (at u (format "        const BufferId buffer_~a = graph.importBuffer<~a>(~a, upload_~a);" n (cpp-type (g-upload-type u)) (cpp-string n) n))
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
                            (format "FixedExtent{zgal::extentOf(context, ~a, ~a)}" (imported-image (entry-item (hash-ref env import))) (cpp-string import))])))]
        [(g-buffer n t count frames when _)
         (values (format "buffer_~a" n) "buffers" when
                 (if (per-frame-expression? count env)
                     ;; Sized each frame: the graph recreates it when its size changes
                     (format "graph.createBuffer(~a, ~a, [~a](const FrameContext& frameContext) { const zgal::FrameInfo frame = zgal::frameInfo(frameContext); return VkDeviceSize(~a) * sizeof(~a); })"
                             (cpp-string n) (frame-mode frames) (frame-captures count env)
                             (build-expr count (scalar-t 'uint)) (cpp-type t))
                     (format "graph.createBuffer(~a, ~a, VkDeviceSize(~a) * sizeof(~a))" (cpp-string n) (frame-mode frames)
                             (build-expr count (scalar-t 'uint)) (cpp-type t))))]))
    (define id-type-name (if (g-image? item) "ImageId" "BufferId"))
    (cond
      [when
       (at item (format "        std::optional<~a> ~a;" id-type-name variable))
       (at item (format "        if (~a) {" (build-expr when (scalar-t 'bool))))
       (at item (format "            ~a = ~a;" variable create))
       (gen "        }")]
      [else (at item (format "        const ~a ~a = ~a;" id-type-name variable create))])
    (unless (internal-name? (graph-item-name item))
      (gen (format "        result.~a.~a = ~a;" kind-name (graph-item-name item) variable))))
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
    ;; The uniforms it binds (alone or in arrays) are written before it records
    (define bound
      (remove-duplicates
       (append-map (λ (pair) (match (cdr pair) [(list rs ...) rs] [r (list r)])) (g-material-bindings mi))))
    (define updates
      (for/list ([r (in-list bound)] #:when (eq? (entry-kind (hash-ref env r)) 'uniform))
        (format "[uniform = uniform_~a](const zgal::FrameInfo& frame) { uniform->update(frame); }" r)))
    ;; The entry of a resource for a binding of kind
    (define (entry-of kind resource)
      (match* (kind (entry-kind (hash-ref env resource)))
        [('texture _) (format "graph.sampledEntry(~a)" (resource-ref resource))]
        [('storage-image _) (format "graph.storageEntry(~a)" (resource-ref resource))]
        [('sampler _) (format "context.descriptors().sampler(sampler_~a)" resource)]
        [(_ 'uniform) (format "uniform_~a->entries()" resource)]
        [(_ (or 'buffer 'import-buffer 'upload)) (format "graph.storageBufferEntry(~a)" (resource-ref resource))]))
    (define entries
      (for/list ([pair (in-list (g-material-bindings mi))])
        (match-define (cons b resource) pair)
        (format ".~a = ~a" (binding-name b)
                (if (eq? (binding-kind b) 'array)
                    (format "array_~a_~a" instance (binding-name b))
                    (entry-of (binding-kind b) resource)))))
    ;; Arrays' entries
    (for ([pair (in-list (g-material-bindings mi))] #:when (eq? (binding-kind (car pair)) 'array))
      (match-define (cons b resource) pair)
      (define kind (binding-resource-kind b))
      (define variable (format "array_~a_~a" instance (binding-name b)))
      (cond
        [(list? resource)
         (line (format "        TRY_ASSIGN(auto ~a, DescriptorArray::create(context, {~a}));" variable
                       (string-join (for/list ([r (in-list resource)]) (entry-of kind r)) ", ")))]
        [else
         ;; Imported images: as many as C++ gives
         (line (format "        std::vector<FrameBinding<DescriptorEntry>> elements_~a_~a;" instance (binding-name b)))
         (line (format "        for (const ImageId image : images_~a) {" resource))
         (line (format "            elements_~a_~a.push_back(graph.~a(image));" instance (binding-name b)
                       (if (eq? kind 'texture) "sampledEntry" "storageEntry")))
         (line "        }")
         (line (format "        TRY_ASSIGN(auto ~a, DescriptorArray::create(context, std::move(elements_~a_~a)));"
                       variable instance (binding-name b)))]))
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
    ;; Graphics materials don't dispatch, nor do compute materials only dispatched indirectly;
    ;; others' workgroups are computed each frame
    (define dispatch
      (if (or graphics? (not (g-material-invocations mi)))
          "nullptr"
          (format "[~a](const zgal::FrameInfo& frame) { return ~a::workgroupsFor(~a); }"
                  (frame-captures (g-material-invocations mi) env) type
                  (string-join (for/list ([e (in-list (g-material-invocations mi))]) (build-expr e (scalar-t 'uint))) ", "))))
    ;; Push constants nodes give (and the instance leaves out) are pushed by those nodes
    (define push? (pair? (material-push m)))
    (define given-push (filter cdr (g-material-push mi)))
    (line (format "        TRY_ASSIGN(auto shaders_~a, ~a::loadShaders(context));" instance type))
    (line (format "        auto material_~a = context.resources().create<zgal::~a<~a>>(" instance (if push? "PushInstance" "Instance") type))
    (line (format "            zgal::Updates{~a}," (string-join updates ", ")))
    (when push?
      (line (format "            [~a](const zgal::FrameInfo& frame, ~a::Push& push) {"
                    (frame-captures (map cdr given-push) env) type))
      (for ([pair (in-list given-push)])
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
    (define material-type
      (and (g-node-material n) (material-name (g-material-material (entry-item (hash-ref env (g-node-material n)))))))
    (define overrides (g-node-overrides n))
    ;; Pushes the node's own push constants, in a lambda holding `material` and `record`
    (define (push-override)
      (format "zgal::pushOverride(record, *material, [&](const zgal::FrameInfo& frame, ~a::Push& push) { ~a });" material-type
              (string-join (for/list ([pair (in-list (node-overrides-push overrides))])
                             (format "push.~a = ~a;" (field-name (car pair)) (cpp-expr (cdr pair) env (field-type (car pair)))))
                           " ")))
    (define (has-push?) (and overrides (pair? (node-overrides-push overrides))))
    (define label (cpp-string node-name))
    ;; An indirect command's place: a BufferOffset
    (define (command-offset ref)
      (format "BufferOffset{~a, VkDeviceSize(~a) * sizeof(~a)}" (resource-ref (element-ref-buffer ref))
              (build-expr (element-ref-index ref) (scalar-t 'uint)) (cpp-type (entry-type (hash-ref env (element-ref-buffer ref))))))
    (define spec (and (indirect? (g-node-extra n)) (g-node-extra n)))
    ;; A draw-meshes' commands draw ranges of its model's mesh
    (define (draw-desc commands)
      (format "IndirectDrawNode::Desc{.material = material_~a, .target = target_~a, .commands = ~a, .maxDraws = ~a~a}"
              (g-node-material n) (g-node-target n) commands (build-expr (indirect-draws spec) (scalar-t 'uint))
              (if (eq? (g-node-kind n) 'draw-meshes) (format ", .mesh = model_~a.mesh" (g-node-subject n)) "")))
    (define counted? (and spec (indirect-count spec) #t))
    (define create
      (match (g-node-kind n)
        ['dispatch
         #:when overrides
         (define push-exprs (map cdr (node-overrides-push overrides)))
         (define invocations (node-overrides-invocations overrides))
         (format "graph.add<zgal::OverridingDispatchNode>(~a, material_~a, ~a, ~a, ~a)" label (g-node-material n)
                 (if (has-push?)
                     (format "[~a, material = material_~a](const RecordContext& record) { ~a }" (frame-captures push-exprs env)
                             (g-node-material n) (push-override))
                     "nullptr")
                 (if invocations
                     (format "[~a](const zgal::FrameInfo& frame) { return ~a::workgroupsFor(~a); }" (frame-captures invocations env) material-type
                             (string-join (for/list ([e (in-list invocations)]) (build-expr e (scalar-t 'uint))) ", "))
                     "nullptr")
                 (if spec (format "std::optional<BufferOffset>(~a)" (command-offset (indirect-command spec))) "std::nullopt"))]
        ['dispatch
         #:when spec
         (format "graph.add<DispatchNode>(~a, material_~a, ~a)" label (g-node-material n) (command-offset (indirect-command spec)))]
        ['dispatch (format "graph.add<DispatchNode>(~a, material_~a)" label (g-node-material n))]
        [(or 'draw 'draw-meshes)
         #:when counted?
         (format "zgal::addCountedDraw(graph, ~a, ~a, ~a, material_~a, ~a)" label
                 (draw-desc (command-offset (indirect-command spec))) (command-offset (indirect-count spec))
                 (indirect-limit spec) (resource-ref (indirect-limited spec)))]
        [(or 'draw 'draw-meshes)
         #:when spec
         (format "graph.add<IndirectDrawNode>(~a, ~a)" label (draw-desc (command-offset (indirect-command spec))))]
        ['draw-meshes (format "graph.add<RenderablesNode>(~a, std::move(renderables), target_~a)" label (g-node-target n))]
        ['draw
         (format "graph.addGraphics(~a, [target = target_~a, material = material_~a](NodeBuilder& builder) { builder.renderTo(target).uses(material); }, [~amaterial = material_~a, count = ~a](RecordContext& record) { material->bind(record.commandBuffer, record.frameIndex); ~avkCmdDraw(record.commandBuffer, count, 1, 0, 0); })"
                 label (g-node-target n) (g-node-material n)
                 (if (has-push?) (string-append (frame-captures (map cdr (node-overrides-push overrides)) env) ", ") "")
                 (g-node-material n)
                 (build-expr (g-node-count n) (scalar-t 'uint))
                 (if (has-push?) (string-append (push-override) " ") ""))]
        ['readback (format "graph.add<~a>(~a, ~a)" (if (image-kind? (g-node-subject n)) "ReadbackNode" "BufferReadbackNode")
                           label (resource-ref (g-node-subject n)))]
        ['clear
         (format "graph.addTransfer(~a, [target = ~a](NodeBuilder& builder) { builder.write(target, Access::TransferDst); }, [target = ~a, value = ~a](RecordContext& record) { vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(target, record.frameIndex), 0, VK_WHOLE_SIZE, value); })"
                 label (resource-ref (first (g-node-writes n))) (resource-ref (first (g-node-writes n)))
                 (build-expr (g-node-count n) (scalar-t 'uint)))]
        ['cpu
         (define uses
           (append (for/list ([b (in-list (g-node-reads n))]) (format ".read(~a, Access::HostRead)" (resource-ref b)))
                   (for/list ([b (in-list (g-node-writes n))]) (format ".write(~a, Access::HostWrite)" (resource-ref b)))))
         (format "graph.addCpu(~a, ~a, in->~a)" label
                 (if (null? uses)
                     "nullptr"
                     (format "[~a](NodeBuilder& builder) { builder~a; }"
                             (string-join (for/list ([b (in-list (remove-duplicates (append (g-node-reads n) (g-node-writes n))))])
                                            (format "buffer_~a" b))
                                          ", ")
                             (string-join uses "")))
                 (g-node-subject n))]
        ['copy
         #:when (image-kind? (first (g-node-writes n)))
         (define source (first (g-node-reads n)))
         (define offset (cdr (g-node-offsets n)))
         (define extent (g-node-count n))
         (format "graph.addTransfer(~a, [source = ~a, destination = ~a](NodeBuilder& builder) { builder.read(source, Access::TransferSrc).write(destination, Access::TransferDst); }, [source = ~a, destination = ~a, sourceOffset = VkDeviceSize(~a) * sizeof(~a), offset = VkOffset2D{int32_t(~a), int32_t(~a)}, extent = ~a](RecordContext& record) { zgal::copyBufferToImage(record, source, destination, sourceOffset, offset, extent, ~a); })"
                 label (resource-ref source) (resource-ref (first (g-node-writes n)))
                 (resource-ref source) (resource-ref (first (g-node-writes n)))
                 (build-expr (car (g-node-offsets n)) (scalar-t 'uint)) (cpp-type (entry-type (hash-ref env source)))
                 (build-expr (first offset) (scalar-t 'uint)) (build-expr (second offset) (scalar-t 'uint))
                 (if extent
                     (format "std::optional<VkExtent2D>(VkExtent2D{~a, ~a})" (build-expr (first extent) (scalar-t 'uint)) (build-expr (second extent) (scalar-t 'uint)))
                     "std::optional<VkExtent2D>()")
                 (cpp-string node-name))]
        ['copy
         (define source (first (g-node-reads n)))
         (define destination (first (g-node-writes n)))
         (define element (cpp-type (entry-type (hash-ref env source))))
         (format "graph.addTransfer(~a, [source = ~a, destination = ~a](NodeBuilder& builder) { builder.read(source, Access::TransferSrc).write(destination, Access::TransferDst); }, [source = ~a, destination = ~a, sourceOffset = VkDeviceSize(~a), destinationOffset = VkDeviceSize(~a), count = ~a](RecordContext& record) { zgal::copyBuffer(record, source, destination, sizeof(~a), sourceOffset, destinationOffset, count, ~a); })"
                 label (resource-ref source) (resource-ref destination) (resource-ref source) (resource-ref destination)
                 (build-expr (car (g-node-offsets n)) (scalar-t 'uint)) (build-expr (cdr (g-node-offsets n)) (scalar-t 'uint))
                 (if (g-node-count n) (format "std::optional<VkDeviceSize>(~a)" (build-expr (g-node-count n) (scalar-t 'uint))) "std::optional<VkDeviceSize>()")
                 element (cpp-string node-name))]
        ['kernel
         (define steps
           (for/list ([step (in-list (g-node-extra n))])
             (define instance (k-step-material step))
             (if (k-step-pass step)
                 (let ([type (material-name (g-material-material (entry-item (hash-ref env instance))))])
                   (format "DispatchSequenceNode::withPushedUint(material_~a, ~a::kPushOffset + offsetof(~a::Push, pass), ~au)"
                           instance type type (k-step-pass step)))
                 (format "{material_~a}" instance))))
         (format "graph.add<DispatchSequenceNode>(~a, std::vector<DispatchSequenceNode::Step>{~a})"
                 label (string-join steps ", "))]
        ['extern #f]))
    (line (if (g-node-when n)
              (format "        if (~a) {" (build-expr (g-node-when n) (scalar-t 'bool)))
              "        {"))
    (when (and (eq? (g-node-kind n) 'draw-meshes) (not spec))
      (cond
        [(eq? (entry-kind (hash-ref env (g-node-subject n))) 'model)
         (line (format "            std::vector<Handle<Renderable>> renderables = model_~a.renderables(context, material_~a);"
                       (g-node-subject n) (g-node-material n)))]
        [else
         (line "            std::vector<Handle<Renderable>> renderables;")
         (line (format "            for (const auto& mesh : in->~a) {" (g-node-subject n)))
         (line (format "                renderables.push_back(context.resources().create<MeshRenderable>(mesh, material_~a));" (g-node-material n)))
         (line "            }")]))
    (cond
      [(eq? (g-node-kind n) 'extern)
       ;; C++ adds the nodes, with the ids so far (its nodes may use the graph's resources)
       (line (format "            result.nodes.~a = in->~a(graph, result);" node-name (g-node-subject n)))]
      [counted?
       ;; The draw, after the node limiting its commands where the device can't count: both
       ;; enabled and run alike
       (line (format "            const zgal::CountedDraw counted = ~a;" create))
       (line "            for (const NodeId node : counted.nodes()) {")
       (when (g-node-enabled n)
         (line (format "                graph.setEnabled(node, ~a);" (build-expr (g-node-enabled n) (scalar-t 'bool)))))
       (when (g-node-once? n)
         (line "                graph.runOnce(node);"))
       (line "            }")
       (line (format "            result.nodes.~a = counted.draw;" node-name))]
      [else
       (line (format "            const NodeId node = ~a;" create))
       (when (g-node-enabled n)
         (line (format "            graph.setEnabled(node, ~a);" (build-expr (g-node-enabled n) (scalar-t 'bool)))))
       (when (g-node-once? n)
         (line "            graph.runOnce(node);"))
       (line (format "            result.nodes.~a = node;" node-name))])
    (line "        }"))
  (gen "        return result;")
  (gen "    }")
  (emit! w "};" #:at (graph-loc g)))

;; A literal expression's value, or #f
(define (e-literal-value* e) (and (e-literal? e) (e-literal-value e)))

(define (item-loc item)
  (match item
    [(g-parameter _ _ l) l] [(g-input _ _ _ l) l] [(g-import _ _ l) l] [(g-image-file _ _ _ l) l] [(g-model _ _ _ l) l] [(g-model-textures _ _ l) l] [(g-model-factors _ _ l) l] [(g-model-draws _ _ _ l) l] [(g-model-bounds _ _ l) l] [(g-import-images _ _ l) l] [(g-import-buffer _ _ l) l] [(g-upload _ _ _ l) l] [(g-constant _ _ _ l) l]
    [(g-sampler _ _ l) l] [(g-uniform _ _ _ l) l] [(g-image _ _ _ _ _ l) l] [(g-buffer _ _ _ _ _ l) l]
    [(g-target _ _ l) l] [(g-material _ _ _ _ _ _ l) l] [(g-node _ _ _ _ _ _ _ _ _ _ _ _ _ _ l) l]))
