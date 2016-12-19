--TEST--
Object test, __sleep
--SKIPIF--
<?php
if (version_compare(PHP_VERSION, '5.2.0') < 0) {
    echo "skip tests in PHP 5.2 or newer";
}
--FILE--
<?php
if(!extension_loaded('swoole_serialize')) {
    dl('swoole_serialize.' . PHP_SHLIB_SUFFIX);
}

function test($type, $variable, $test) {
    $serialized = swoole_serialize($variable);
    $unserialized = swoole_unserialize($serialized);

    echo $type, PHP_EOL;
     
    var_dump($unserialized);
    echo $test || $unserialized == $variable ? 'OK' : 'ERROR', PHP_EOL;
}

class Obj {
    public $a;
    protected $b;
    private $c;
    var $d;

    function __construct($a, $b, $c, $d) {
        $this->a = $a;
        $this->b = $b;
        $this->c = $c;
        $this->d = $d;
    }

    function __sleep() {
        return array('a', 'b', 'c');
    }

#   function __wakeup() {
#       $this->d = $this->a + $this->b + $this->c;
#   }
}

$o = new Obj(1, 2, 3, 4);


test('object', $o, true);
?>
--EXPECTF--
object
object(Obj)#%d (4) {
  ["a"]=>
  int(1)
  [%r"?b"?:protected"?%r]=>
  int(2)
  [%r"?c"?:("Obj":)?private"?%r]=>
  int(3)
  ["d"]=>
  NULL
}
OK
