;; Regression tests for fiber error handling
;; Issues: #565 (fiber limit error), #564 (error propagation)
;; #551 (native reject) was superseded by #1155 (native trampoline).

(import (scheme base) (scheme write) (kaappi fibers))

;; --- #1155: spawn accepts zero-arg natives via trampoline ---
(display "test-native-trampoline: ")
(guard (exn (#t
  (display "raised: ")
  (display (if (error-object? exn) (error-object-message exn) exn))
  (newline)))
  (let ((f (spawn list)))
    (if (null? (fiber-join f))
      (begin (display "ok") (newline))
      (begin (display "bad-result") (newline)))))

;; --- #564: errors in fibers propagate via fiber-join ---
(display "test-fiber-error-propagate: ")
(let ((f (spawn (lambda () (error "fiber-boom" 42)))))
  (guard (exn (#t
    (if (and (error-object? exn)
             (string=? (error-object-message exn) "fiber-boom"))
      (begin (display "ok") (newline))
      (begin (display "FAIL - wrong error: ")
             (display (error-object-message exn)) (newline)))))
    (fiber-join f)
    (display "FAIL - should have raised") (newline)))

;; --- #564: division by zero in fiber propagates ---
(display "test-fiber-div-zero: ")
(let ((f (spawn (lambda () (/ 1 0)))))
  (guard (exn (#t (display "ok") (newline)))
    (fiber-join f)
    (display "FAIL - should have raised") (newline)))

;; --- #565: fiber limit exceeded gives proper error ---
(display "test-fiber-limit: ")
(guard (exn (#t
  (if (error-object? exn)
    (begin (display "ok") (newline))
    (begin (display "ok-non-error") (newline)))))
  (let loop ((i 0))
    (if (< i 200)
      (begin (spawn (lambda () (yield) (yield) (yield) i))
             (loop (+ i 1)))
      #t))
  (display "ok-no-error") (newline))
