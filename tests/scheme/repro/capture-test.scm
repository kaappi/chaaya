(import (scheme base))
(include "harness.scm")

(define-syntax capture-test
  (syntax-rules ()
    ((capture-test exp)
     (let ((x 100))
       exp))))

(check-eqv "hygiene no capture" 1
  (let ((x 1))
    (capture-test x)))
(check-finish)
