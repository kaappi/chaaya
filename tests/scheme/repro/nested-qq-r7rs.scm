(import (scheme base))

(let ((name1 'x)
      (name2 'y))
  (display (equal? '(a `(b ,x ,'y d) e)
                   `(a `(b ,,name1 ,',name2 d) e)))
  (newline))
