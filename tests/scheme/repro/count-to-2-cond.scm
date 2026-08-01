(import (scheme base) (chibi test))

(test-begin "4.3 Macros")

(define-syntax count-to-2
  (syntax-rules ()
    ((_ ) 0)
    ((_ _) 1)
    ((_ _ _) 2)
    ((_ . _) 'many)))

(test '(2 0 many)
  (list (count-to-2 a b) (count-to-2) (count-to-2 a b c d)))

(test 'ok (let ((=> #f)) (cond (#t => 'ok))))

(test-end)
