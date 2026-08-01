(import (scheme base))
(define-syntax capture-test
  (syntax-rules ()
    ((capture-test exp)
     (let ((x 100))
       exp))))
(display (let ((x 1))
            (capture-test x)))
(newline)
