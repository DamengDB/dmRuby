## dmRuby

此包为Ruby连接达梦数据库的原生驱动。dmRuby gem包下载地址：https://rubygems.org/gems/ruby-dm。

## Extensions

-   支持Rails框架

## Change Logs

- dmRuby v1.0.7

  1、修改merge语句影响行数错误的问题。

- dmRuby v1.0.6

  1、修改读取中文数据缺失的问题；

  2、修改插入带时区的时间数据没有转换时区的问题；

  3、修改插入空数据的问题；

  4、修改读取空串的问题；

- dmRuby v1.0.5

  1、修改读取bit类型结果错误的问题；

  2、修改Dm::Client.affected_rows()返回结果错误的问题；

  3、修改对number类型，查询sum()求和返回结果丢失精度的问题；

  4、修改不手动调用stmt.close，内存持续增长的问题；

  5、修改blob插入二进制数据，数据库查询为空的问题；

  6、修改插入较长的整数会溢出的问题。

- dmRuby v1.0.4

  1、新增连接参数schema。

- dmRuby v1.0.3

  1、修改stmt执行的语句不能输出array类型的result。

- dmRuby v1.0.2

  1、修改读取blob数据错误的问题。

- dmRuby v1.0.1

  1、增加client.ping函数用来判断连接是否存活。

- dmRuby v1.0.0

  1、修改返回类型名不完整的问题;

  2、修改fields方法返回列名未区分大小写的问题;

  3、修改binary类型数据绑定方式插入后结果错误的问题;

  4、修改设置encoding无效的问题;

  5、修改encoding = 5时，返回编码错误的问题。