(import (scheme base) (chibi test))

(test-begin "4.3 Macros")

(test 'ok (let ((=> #f)) (cond (#t => 'ok))))

(test-end)
