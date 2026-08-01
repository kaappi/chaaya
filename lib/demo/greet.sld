(define-library (demo greet)
  (export greet double)
  (import (scheme base))
  (begin
    (define (greet name)
      (string-append "hello " name))
    (define (double x)
      (* x 2))))
