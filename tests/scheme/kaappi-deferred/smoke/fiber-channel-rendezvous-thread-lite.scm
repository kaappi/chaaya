;; Minimal cross-thread channel smoke (Chaaya Phase E).
(import (scheme base) (scheme write) (srfi 18) (kaappi fibers))

(let ((ch (make-channel)))
  (let ((t (thread-start!
            (make-thread
             (lambda ()
               (channel-send ch 42)
               'done)))))
    (unless (eqv? (channel-receive ch) 42)
      (error "bad payload"))
    (unless (eq? (thread-join! t) 'done)
      (error "bad join"))
    (display "ok")
    (newline)))
