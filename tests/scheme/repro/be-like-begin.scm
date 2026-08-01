(import (scheme base) (chibi test))

(define-syntax be-like-begin1
  (syntax-rules ()
    ((be-like-begin1 name)
     (define-syntax name
       (syntax-rules ()
         ((name expr (... ...))
          (begin expr (... ...))))))))
(be-like-begin1 sequence1)
(test-begin "seq")
(test 3 (sequence1 0 1 2 3))
(test-end "seq")
