using System.Runtime.InteropServices;

namespace PersonalIME.Manager.Ipc;

public enum AdminErrorCode
{
    Success = 0,
    InvalidMagic = 1001,
    UnsupportedVersion = 1002,
    PayloadTooLarge = 1003,
    InvalidJson = 1004,
    UnknownAction = 1005,
    MissingParameter = 1006,
    ValidationFailed = 1007,
    EntryNotFound = 2001,
    StorageError = 3001,
    Busy = 3002,
    RevisionConflict = 4001
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct AdminHeader
{
    public const uint MagicValue = 0x414D4950; // 'P','I','M','A' in little-endian
    public const uint ProtocolVersion1 = 0x0100;
    public const uint MaxPayloadSize = 16 * 1024 * 1024; // 16 MB
    public const int HeaderSize = 16;

    public byte Magic0;
    public byte Magic1;
    public byte Magic2;
    public byte Magic3;
    public uint Version;
    public uint PayloadLength;
    public uint RequestId;

    public static AdminHeader Create(uint requestId, uint payloadLength)
    {
        return new AdminHeader
        {
            Magic0 = (byte)'P',
            Magic1 = (byte)'I',
            Magic2 = (byte)'M',
            Magic3 = (byte)'A',
            Version = ProtocolVersion1,
            PayloadLength = payloadLength,
            RequestId = requestId
        };
    }

    public bool IsValidMagic()
    {
        return Magic0 == 'P' && Magic1 == 'I' && Magic2 == 'M' && Magic3 == 'A';
    }

    public static AdminHeader ReadFrom(ReadOnlySpan<byte> buffer)
    {
        if (buffer.Length < HeaderSize)
            throw new ArgumentException("Buffer too small for AdminHeader");

        return MemoryMarshal.Read<AdminHeader>(buffer[..HeaderSize]);
    }

    public void WriteTo(Span<byte> buffer)
    {
        if (buffer.Length < HeaderSize)
            throw new ArgumentException("Buffer too small for AdminHeader");

        MemoryMarshal.Write(buffer[..HeaderSize], in this);
    }
}

