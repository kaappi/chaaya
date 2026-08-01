(import (scheme base) (scheme complex) (scheme inexact) (scheme read) (scheme write))

(define (test expect got)
  (unless (equal? expect got)
    (error "fail" expect got)))

(test #t (complex? 3+4i))
(test #t (complex? 3))
(test #t (real? 3))
(test #t (real? -2.5+0i))
(test #f (real? -2.5+0.0i))
(test #t (real? #e1e10))
(test #t (real? +inf.0))
(display "ok\n")
