;; Phase 10 MVP: cooperative fibers/channels + SRFI-18 NYI stubs.

(import (scheme base))
(import (chaaya fibers))

(define ch (make-channel))
(define marks '())

(spawn-fiber
 (lambda ()
   (set! marks (cons 'started marks))
   (channel-send! ch 42)
   (set! marks (cons 'finished marks))
   'done))

(check-eqv "channel-recv drives spawned fiber" 42 (channel-recv ch))
(check-assert "spawned fiber reached end" (memq 'finished marks))

(define ch2 (make-channel))
(spawn-fiber (lambda () (channel-send! ch2 'ok)))
(fiber-yield)
(check-eq "fiber-yield runs one ready fiber" 'ok (channel-recv ch2))

(check-finish)
