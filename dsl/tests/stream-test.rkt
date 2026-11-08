#lang racket/base
;; Stream buffers: buffers a stream input fills over frames, their headers (stream-header), and the
;; streamed event apps get as their bytes arrive

(require racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (named outputs suffix)
  (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) suffix)) o)))

(define streamed
  (string-append
   "(struct Header (magic uint) (words uint) (scale float))\n"
   "(compute-material Scale (workgroup 1) (bindings (values (buffer-array uint)) (results (buffer-array uint))) (push (count uint) (scale float))\n"
   "  (compute (write! (at results 0) (uint (* (float (at values 0)) scale)))))\n"
   "(graph G (input open (stream Header)) (input file path)\n"
   "  (stream-buffer scene open file #:words (.words (stream-header scene)) #:per-frame 64)\n"
   "  (constant n (.words (stream-header scene)))\n"
   "  (buffer results uint 1 #:frames shared)\n"
   "  (material scale Scale (values scene) results #:push ((count n) (scale (.scale (stream-header scene)))) #:invocations (1))\n"
   "  (dispatch run scale)\n"
   "  (readback read scene))\n"))

(define app
  (string-append
   streamed
   "(app A (state (got uint 0) (size uint 0))\n"
   "  (on (streamed scene arrived total) (with state #:got arrived #:size total))\n"
   "  (run G))\n"))

(test-case "A stream buffer opens its stream and reads its header when the graph is built, and is sized from it"
  (define h (named (compile-text streamed) ".zgal.h"))
  (check-true (string-contains? h "zgal::StreamOpener open;"))
  (check-true (string-contains? h "contractViolation(\"G's input open isn't given\");"))
  (check-true (string-contains? h "TRY_ASSIGN(auto stream_scene, zgal::openStream<Header>(in->open, in->file).withContext(\"loading G's stream-buffer scene\"));"))
  (check-true (string-contains? h "[[maybe_unused]] const Header header_scene = stream_scene.header;"))
  (check-true (string-contains? h "const BufferId buffer_scene = graph.createStreamBuffer(\"scene\", std::move(stream_scene.stream), VkDeviceSize(header_scene.words), VkDeviceSize(64u));"))
  (check-true (string-contains? h "BufferId scene;"))
  ;; Its header is a build-time value: constants after it, and push constants, read it
  (check-true (regexp-match? #rx"header_scene = .*const uint32_t constant_n = header_scene.words;" h))
  (check-true (string-contains? h "push.scale = header_scene.scale;"))
  ;; Shaders bind it as a buffer of uints, and readbacks read it
  (check-true (string-contains? h ".values = graph.storageBufferEntry(buffer_scene)"))
  (check-true (string-contains? h "graph.add<BufferReadbackNode>(\"read\", buffer_scene)"))
  ;; A string path, and #:words and #:per-frame from parameters (checked when the graph is built)
  (define literal (string-replace (string-replace streamed "open file #:words" "open \"scene.stream\" #:words")
                                  "#:per-frame 64)" "#:per-frame (* chunk 4))"))
  (define with-parameter (string-replace literal "(input file path)" "(parameter chunk uint)"))
  (define literal-h (named (compile-text with-parameter) ".zgal.h"))
  (check-true (string-contains? literal-h "zgal::openStream<Header>(in->open, std::filesystem::path(\"scene.stream\"))"))
  (check-true (string-contains? literal-h "VkDeviceSize((in->chunk * 4u))")))

(test-case "Stream inputs and buffers are checked"
  (check-zgal-error (string-replace streamed "(stream Header)" "(stream (array Header 2))") 'type-not-allowed "give the element type")
  (check-zgal-error (string-replace streamed "(stream Header)" "(stream bool)") 'type-not-allowed "a stream's header")
  (check-zgal-error (string-replace streamed " #:words (.words (stream-header scene))" "") 'bad-option "stream-buffer needs #:words")
  (check-zgal-error (string-replace streamed " #:per-frame 64" "") 'bad-option "stream-buffer needs #:per-frame")
  (check-zgal-error (string-replace streamed "#:per-frame 64" "#:per-frame 6") 'bad-option "#:per-frame is a positive multiple of 4" "not 6")
  (check-zgal-error (string-replace streamed "#:per-frame 64" "#:per-frame 0") 'bad-option "not 0")
  (check-zgal-error (string-replace streamed "#:per-frame 64" "#:per-frame 64 #:when #t") 'bad-option "#:when")
  (check-zgal-error (string-replace streamed "#:per-frame 64" "#:per-frame 1.5") 'type-mismatch "#:per-frame")
  (check-zgal-error (string-replace streamed "#:words (.words (stream-header scene))" "#:words (stream-header scene)") 'type-mismatch "#:words" "Header")
  (check-zgal-error (string-replace streamed "(stream-buffer scene open file" "(stream-buffer scene file file") 'type-mismatch
                    "file is a path input, not a stream input")
  (check-zgal-error (string-replace streamed "(stream-buffer scene open file" "(stream-buffer scene open 3") 'malformed "an asset's path")
  (check-zgal-error (string-replace streamed "(stream-buffer scene open file #:words" "(stream-buffer scene open #:words") 'malformed
                    "(stream-buffer name stream-input asset #:words n #:per-frame bytes)")
  ;; The header is a stream buffer's, and a build-time value
  (check-zgal-error (string-replace streamed "(constant n (.words (stream-header scene)))" "(constant n (.words (stream-header results)))")
                    'unknown-name "results isn't declared")
  (check-zgal-error (string-replace streamed "(constant n (.words (stream-header scene)))" "(constant n (.words (stream-header open)))")
                    'type-mismatch "takes a stream-buffer, and open isn't one")
  (check-zgal-error (string-replace streamed "(constant n (.words (stream-header scene)))" "(constant n (stream-header scene 1))")
                    'malformed "takes 1 argument")
  (check-zgal-error (string-replace (string-replace streamed "(input file path)" "(input file path) (input w (per-frame uint))")
                                    "#:words (.words (stream-header scene))" "#:words w")
                    'not-build-time)
  ;; Used as a value, a stream input isn't one
  (check-zgal-error (string-replace streamed "(constant n (.words (stream-header scene)))" "(constant n open)")
                    'type-mismatch "open is a stream input, not a value"))

(test-case "Apps get a streamed event before a frame whose stream buffer's bytes changed"
  (define h (named (compile-text app) ".zgal.h"))
  ;; The app gives the opener through, as C++ gives it
  (check-true (string-contains? h ".open = std::move(app->inputs_.open),"))
  ;; Streamed sizes are uints, so create refuses a buffer they'd wrap
  (check-true (string-contains? h "if (const VkDeviceSize bytes = app->graph0_->bufferSize(app->ids0_->buffers.scene, 0); bytes > UINT32_MAX) {"))
  (check-true (string-contains? h "so it must be under 4 GiB\");"))
  (check-true (string-contains? h "TRY(graph0_->readStreams());"))
  (check-true (string-contains? h "const VkDeviceSize arrived = graph0_->streamArrived(ids0_->buffers.scene);"))
  (check-true (string-contains? h "if (arrived != arrived0_) {"))
  (check-true (string-contains? h "state_ = on0(state_, static_cast<uint32_t>(arrived), static_cast<uint32_t>(graph0_->bufferSize(ids0_->buffers.scene, context_->currentFrame())));"))
  (check-true (string-contains? h "State on0([[maybe_unused]] const State& state, [[maybe_unused]] uint32_t arrived, [[maybe_unused]] uint32_t total) {"))
  (check-true (string-contains? h "VkDeviceSize arrived0_ = 0;"))
  ;; Before the frame handler, so it sees the new state
  (define with-frame (string-replace app "(run G))" "(on (frame dt) state) (run G))"))
  (check-true (regexp-match? #rx"state_ = on0[(].*state_ = on1[(]state_, inputs_.timestep.next[(][)][)];" (named (compile-text with-frame) ".zgal.h")))
  ;; Errors
  (check-zgal-error (string-replace app "(streamed scene arrived total)" "(streamed results arrived total)") 'type-mismatch
                    "results in graph G isn't a stream-buffer")
  (check-zgal-error (string-replace app "(streamed scene arrived total)" "(streamed nothing arrived total)") 'unknown-name
                    "no graph app A runs has a stream-buffer nothing")
  (check-zgal-error (string-replace app "(streamed scene arrived total)" "(streamed scene arrived)") 'malformed
                    "expected (streamed buffer arrived total)")
  (check-zgal-error (string-replace app "(streamed scene arrived total)" "(streamed scene arrived arrived)") 'duplicate-name
                    "binds arrived twice")
  (check-zgal-error (string-replace app "(run G))" "(on (streamed scene a b) state) (run G))") 'duplicate-name
                    "streamed scene is already handled")
  (check-zgal-error (string-replace app "(run G))" "(run G (open 1)))") 'type-mismatch "open is a stream input, which C++ gives"))

(test-case "The web backend refuses stream inputs and buffers until it streams them"
  (define (check-not-on-web text fragment)
    (define e (zgal-error-of text #:web? #t))
    (with-check-info (['program text])
      (check-true (exn:zgal? e))
      (when (exn:zgal? e)
        (check-equal? (exn:zgal-id e) 'not-on-web)
        (check-true (string-contains? (exn-message e) fragment) (exn-message e)))))
  (check-not-on-web app "stream input open (and the stream-buffers it fills) has no web form")
  ;; Native-only programs still compile natively alongside
  (check-false (zgal-error-of app)))
