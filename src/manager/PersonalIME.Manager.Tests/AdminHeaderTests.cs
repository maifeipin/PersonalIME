using PersonalIME.Manager.Ipc;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class AdminHeaderTests
{
    [TestMethod]
    public void TestHeaderExact16Bytes()
    {
        Assert.AreEqual(16, System.Runtime.CompilerServices.Unsafe.SizeOf<AdminHeader>());
    }

    [TestMethod]
    public void TestHeaderSerializationRoundtrip()
    {
        var header = AdminHeader.Create(requestId: 1024, payloadLength: 4096);
        Assert.IsTrue(header.IsValidMagic());
        Assert.AreEqual(AdminHeader.ProtocolVersion1, header.Version);
        Assert.AreEqual((uint)1024, header.RequestId);
        Assert.AreEqual((uint)4096, header.PayloadLength);

        byte[] bytes = new byte[16];
        header.WriteTo(bytes);

        // Check magic bytes
        Assert.AreEqual((byte)'P', bytes[0]);
        Assert.AreEqual((byte)'I', bytes[1]);
        Assert.AreEqual((byte)'M', bytes[2]);
        Assert.AreEqual((byte)'A', bytes[3]);

        // Little-endian check
        uint versionInBytes = BitConverter.ToUInt32(bytes, 4);
        Assert.AreEqual(0x0100u, versionInBytes);

        uint lenInBytes = BitConverter.ToUInt32(bytes, 8);
        Assert.AreEqual(4096u, lenInBytes);

        uint idInBytes = BitConverter.ToUInt32(bytes, 12);
        Assert.AreEqual(1024u, idInBytes);

        // Read back
        var readHeader = AdminHeader.ReadFrom(bytes);
        Assert.IsTrue(readHeader.IsValidMagic());
        Assert.AreEqual(header.Version, readHeader.Version);
        Assert.AreEqual(header.PayloadLength, readHeader.PayloadLength);
        Assert.AreEqual(header.RequestId, readHeader.RequestId);
    }

    [TestMethod]
    public void TestInvalidMagicRejected()
    {
        byte[] corrupted = new byte[16];
        corrupted[0] = (byte)'X';
        corrupted[1] = (byte)'Y';
        corrupted[2] = (byte)'Z';
        corrupted[3] = (byte)'!';

        var header = AdminHeader.ReadFrom(corrupted);
        Assert.IsFalse(header.IsValidMagic());
    }
}
