(define-syntax when
  (syntax-rules ()
    ((when test body ...)
     (if test (begin body ...) #f))))

(when #t 1 2)
