(import (scheme base) (scheme process-context) (srfi 64))
(test-begin "lists")
(test-group "list-copy"
  (test-equal "list-copy produces equal list"
    '(1 2 3)
    (list-copy (list 1 2 3)))
  (test-equal "list-copy is independent of original"
    '(1 2 3)
    (let ((original (list 1 2 3)))
      (let ((copy (list-copy original)))
        (list-set! copy 0 99)
        original)))
  (test-equal "list-copy mutation applies to copy"
    '(99 2 3)
    (let ((original (list 1 2 3)))
      (let ((copy (list-copy original)))
        (list-set! copy 0 99)
        copy))))
(test-group "member"
  (test-equal "member found" '(3 4 5) (member 3 '(1 2 3 4 5))))
(test-end "lists")
