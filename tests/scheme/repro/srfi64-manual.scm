(import (scheme base) (srfi 64))

(test-begin "x")

;; Manual expansion of (test-equal "a" 1 1)
(let* ((r (test-runner-get))
       (name "a"))
  (test-result-alist! r (list (cons 'test-name name)))
  (let ()
    (if (%test-on-test-begin r)
        (let ((exp 1))
          (test-result-expected-value! r exp)
          (let ((res (%test-evaluate-with-catch 1)))
            (test-result-actual-value! r res)
            (%test-on-test-end r (equal? exp res)))))
    (%test-report-result)))

(test-end "x")
