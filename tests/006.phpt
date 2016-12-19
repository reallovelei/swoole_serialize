--TEST--
Check for simple string serialization
--SKIPIF--
--FILE--
<?php
if(!extension_loaded('swoole_serialize')) {
    dl('swoole_serialize.' . PHP_SHLIB_SUFFIX);
}

function test($type, $variable) {
    $serialized = swoole_serialize($variable);
    $unserialized = swoole_unserialize($serialized);

    echo $type, PHP_EOL;
    var_dump($unserialized);
    echo $unserialized === $variable ? 'OK' : 'ERROR', PHP_EOL;
}

test('empty: ""', "");
test('string: "foobar"', "foobar");
?>
--EXPECT--
empty: ""
string(0) ""
OK
string: "foobar"
string(6) "foobar"
OK
