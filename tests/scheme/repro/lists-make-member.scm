(import (scheme base) (scheme process-context) (srfi 64))
(test-begin "lists")
(test-group "make-list"
  (test-equal "make-list with fill" '(0 0 0) (make-list 3 0))
  (test-equal "make-list zero length" '() (make-list 0)))
(test-group "member"
  (test-equal "member found" '(3 4 5) (member 3 '(1 2 3 4 5))))
(test-end "lists")
